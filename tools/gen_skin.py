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
    """Grid: approx w=11, h=21. Polish: bloodstained apron, exposed-muscle arm streaks,
    brighter cleaver-side fist."""
    w, h = 12, 21
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
    # Bloodstained apron band across chest/waist rows — dirty off-white with blood splatters
    for px in range(3, 8):
        p[(px, 8)] = (190, 175, 155, 255)    # apron band top
        p[(px, 9)] = (185, 170, 150, 255)    # apron band
    # Blood stains on the apron
    p[(4, 8)]  = (160, 50,  35, 255)         # blood stain left
    p[(6, 9)]  = (150, 40,  28, 255)         # blood stain right
    p[(5, 8)]  = (140, 135, 115, 255)        # apron center slightly soiled
    # Exposed-muscle streaks on arms (bright red highlights)
    muscle = (210, 60, 40, 255)
    for py in range(5, 14):
        if py % 2 == 0:
            p[(2, py)] = muscle             # left arm muscle streak
            p[(9, py)] = muscle             # right arm muscle streak
    # Brighter cleaver-side fist (right hand, slightly brighter)
    cleaver_fist = (180, 55, 35, 255)
    for py in [4, 5]:
        p[(9,  py)] = cleaver_fist
        p[(10, py)] = cleaver_fist
    # Left fist stays standard
    fist = (120, 30, 20, 255)
    for py in [4, 5]:
        p[(0, py)] = fist; p[(1, py)] = fist
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


def skin_andariel():
    """Andariel boss: spider-woman demon. Polish: glistening egg-sac abdomen highlights,
    darker chitin plates, brighter red eye cluster. Grid unchanged: w=11, h=18."""
    w, h = 11, 18
    p = {}
    chitin = (38, 55, 28, 255)   # slightly darker base chitin
    for py in range(h):
        for px in range(w):
            p[(px, py)] = chitin
    # Head (y15-17) — darker green skull
    for py in range(15, 18):
        for px in range(3, 8): p[(px, py)] = (50, 70, 36, 255)
    # Four red eyes — brighter, more saturated cluster
    p[(4, 16)] = (255, 35, 15, 255)    # upper left — very bright
    p[(6, 16)] = (255, 35, 15, 255)    # upper right
    p[(4, 15)] = (220, 28, 12, 255)    # lower left
    p[(6, 15)] = (220, 28, 12, 255)    # lower right
    # Mouth/fangs — pale bone
    p[(5, 14)] = (210, 200, 168, 255)
    # Chitin shoulder pauldrons — darker plate contrast
    for py in range(10, 13):
        for px in [0, 1, 9, 10]: p[(px, py)] = (28, 45, 20, 255)
    p[(0, 12)] = (22, 36, 16, 255)
    p[(10, 12)] = (22, 36, 16, 255)
    # Upper torso (y10-12) — broad chest plate slightly brighter
    for py in range(10, 13):
        for px in range(2, 9): p[(px, py)] = (48, 72, 36, 255)
    # Narrow waist (y8-9) — dark constriction
    for py in range(8, 10):
        for px in range(4, 7): p[(px, py)] = (32, 48, 24, 255)
    # Abdomen (y4-7) — glistening translucent amber egg-sac highlights
    for py in range(4, 8):
        for px in range(2, 9): p[(px, py)] = (48, 72, 34, 255)
    # Egg-sac glistening highlights — amber/translucent center
    for px in range(3, 8):
        p[(px, 6)] = (160, 120, 50, 255)    # bright amber highlight stripe
    for px in range(4, 7):
        p[(px, 5)] = (130,  95, 40, 255)    # secondary highlight
    p[(5, 7)] = (180, 140, 65, 255)          # peak highlight
    # Arms — dark chitin
    for py in range(6, 12):
        p[(0, py)] = (32, 50, 24, 255)
        p[(10, py)] = (32, 50, 24, 255)
    # Claws — pale bone, slightly more vivid
    p[(0, 5)]  = (195, 182, 148, 255)
    p[(10, 5)] = (195, 182, 148, 255)
    # Legs (y0-3) — darkest chitin
    for py in range(0, 4):
        p[(3, py)] = (30, 44, 22, 255)
        p[(7, py)] = (30, 44, 22, 255)
    p[(3, 0)] = (24, 36, 16, 255)
    p[(7, 0)] = (24, 36, 16, 255)
    return w, h, p


def skin_mephisto():
    """Mephisto/Warden boss: tomb-grey stone armor, spectral blue glow in joints/eyes.
    Grid matches gen_warden: w=13, h=22. min_gx=-6 so pixel col = gx+6."""
    w, h = 13, 22
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 18:   p[(px, py)] = (130, 130, 140, 255)   # skull — pale stone
            elif py >= 16: p[(px, py)] = (100, 100, 110, 255)   # crown/helm top
            elif py >= 12: p[(px, py)] = (80,  85,  95, 255)    # broken crown rows
            elif py >= 7:  p[(px, py)] = (70,  72,  82, 255)    # cuirass — dark stone armor
            elif py >= 6:  p[(px, py)] = (55,  58,  68, 255)    # belt
            elif py >= 2:  p[(px, py)] = (65,  68,  78, 255)    # greaves
            else:          p[(px, py)] = (50,  52,  60, 255)    # sabatons
    # Gravestone pauldron slabs — rusted-iron trim (cols 0-1 and 11-12)
    for py in range(12, 16):
        for px in [0, 1]:   p[(px, py)] = (90,  65,  45, 255)   # left pauldron
        for px in [11, 12]: p[(px, py)] = (90,  65,  45, 255)   # right pauldron
    # Skull eye sockets — spectral blue glow
    p[(5, 18)] = (80, 180, 255, 255)
    p[(7, 18)] = (80, 180, 255, 255)
    # Ribcage gaps — deep shadow with spectral blue edge
    p[(5, 9)]  = (30,  60, 140, 255)
    p[(7, 9)]  = (30,  60, 140, 255)
    p[(5, 11)] = (30,  60, 140, 255)
    p[(7, 11)] = (30,  60, 140, 255)
    # Gauntlet fists — dark iron with blue joint glow
    for py in range(4, 7):
        p[(0,  py)] = (55, 58, 68, 255)
        p[(12, py)] = (55, 58, 68, 255)
    p[(0, 5)]  = (60, 140, 220, 255)   # left joint glow
    p[(12, 5)] = (60, 140, 220, 255)   # right joint glow
    # Broken crown spikes — asymmetric (left tall, right stub missing top)
    for py in range(20, 22):
        p[(4, py)] = (110, 115, 125, 255)   # left crown spike
    p[(6, 20)] = (110, 115, 125, 255)        # center stub
    for py in range(20, 22):
        p[(8, py)] = (110, 115, 125, 255)   # right spike intact
    # p[(8, 21)] left default — matches discarded voxel (broken tip)
    return w, h, p


def skin_baal():
    """Baal/Korvath boss: dark iron plate, bronze trim, glowing red visor slit.
    Grid matches gen_korvath: w=14, h=22. min_gx=-7 so pixel col = gx+7."""
    w, h = 14, 22
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 18:   p[(px, py)] = (45,  42,  55, 255)    # helm — near-black iron
            elif py >= 14: p[(px, py)] = (40,  38,  50, 255)    # upper helm/horns
            elif py >= 12: p[(px, py)] = (50,  48,  60, 255)    # pauldrons
            elif py >= 7:  p[(px, py)] = (55,  52,  65, 255)    # cuirass
            elif py >= 5:  p[(px, py)] = (45,  42,  55, 255)    # fauld/hips
            elif py >= 2:  p[(px, py)] = (50,  48,  60, 255)    # greaves
            else:          p[(px, py)] = (38,  36,  48, 255)    # sabatons
    # Bronze trim highlights on armor edges
    for px in range(3, 11):
        p[(px, 7)]  = (130, 95, 40, 255)   # cuirass lower rim
        p[(px, 13)] = (130, 95, 40, 255)   # pauldron bottom trim
    for py in range(7, 14):
        p[(3,  py)] = (110, 80, 35, 255)   # left side trim
        p[(10, py)] = (110, 80, 35, 255)   # right side trim
    # Battle scratch marks on cuirass
    p[(6, 10)] = (70, 67, 80, 255)
    p[(7, 9)]  = (70, 67, 80, 255)
    p[(8, 11)] = (70, 67, 80, 255)
    # Glowing red visor slit (helm gy=17 → py=17, across gx=-2..3 → col 5..10)
    for px in range(5, 11):
        p[(px, 17)] = (255, 40, 20, 255)   # visor slit bright red glow
    # Horn tips — darker iron
    for py in range(19, 22):
        p[(1, py)] = (35, 32, 42, 255)    # left horn tip
        p[(12, py)] = (35, 32, 42, 255)   # right horn tip
    # Tower shield face (left arm, cols 0-1)
    for py in range(4, 13):
        p[(0, py)] = (62, 58, 72, 255)    # shield plate
        p[(1, py)] = (68, 64, 78, 255)    # shield inner
    p[(0, 8)] = (140, 100, 38, 255)       # shield boss center — bronze
    p[(1, 8)] = (140, 100, 38, 255)
    # Spiked gauntlet right (cols 12-13 lower)
    for py in range(3, 6):
        p[(12, py)] = (55, 52, 65, 255)
        p[(13, py)] = (60, 55, 68, 255)
    p[(13, 4)] = (200, 190, 150, 255)    # spike tip highlight
    return w, h, p


def skin_diablo():
    """Diablo boss: deep crimson + charred black, burning orange cracks, blazing orange eyes.
    Grid matches gen_diablo: w=15, h=26. min_gx=-7 so pixel col = gx+7."""
    w, h = 15, 26
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 22:   p[(px, py)] = (15,  8,   5, 255)    # ram horn sweep — charred black
            elif py >= 18: p[(px, py)] = (145, 20,  8, 255)    # skull — deep crimson
            elif py >= 16: p[(px, py)] = (120, 18,  6, 255)    # neck (hunched)
            elif py >= 10: p[(px, py)] = (160, 28, 12, 255)    # torso
            elif py >= 8:  p[(px, py)] = (90,  15,  6, 255)    # belt
            elif py >= 4:  p[(px, py)] = (130, 22, 10, 255)    # thighs
            elif py >= 2:  p[(px, py)] = (100, 16,  7, 255)    # lower legs
            else:          p[(px, py)] = (60,  10,  4, 255)    # feet
    # Burning orange cracks scattered across body
    crack = (255, 130, 25, 255)
    for (px, py) in [(7,20),(6,17),(8,15),(5,12),(9,11),(7,9),(6,7),(8,5)]:
        p[(px, py)] = crack
    # Ridged spine spikes (back, col 7-8 at odd y rows 9-17)
    for sy in range(9, 18, 2):
        p[(7, sy)] = (55, 10, 5, 255)   # spine ridge dark
        p[(8, sy)] = (55, 10, 5, 255)
    # Blazing orange eyes (gx=-2,-1,0,1 → col 5-8 at gy=20→py=20)
    p[(5, 20)] = (255, 165, 30, 255)
    p[(6, 20)] = (255, 165, 30, 255)
    p[(8, 20)] = (255, 165, 30, 255)
    p[(9, 20)] = (255, 165, 30, 255)
    # Bestial maw — carved open (dark interior at snout rows 15-16)
    for px in range(5, 10):
        p[(px, 15)] = (25, 5, 2, 255)
        p[(px, 16)] = (25, 5, 2, 255)
    # Broad clawed hands
    for py in range(2, 5):
        for px in [0, 1, 2]:   p[(px, py)] = (80, 12, 5, 255)   # left claws
        for px in [12, 13, 14]: p[(px, py)] = (80, 12, 5, 255)  # right claws
    # Ram horn curve — charred with ember tips
    p[(2, 25)] = (255, 80, 10, 255)    # left horn tip ember
    p[(12, 25)] = (255, 80, 10, 255)  # right horn tip ember
    return w, h, p


def skin_reaper():
    """Grim Reaper boss: near-black cloak, bone-white skull, ghostly green-blue glow in eyes.
    Grid matches gen_reaper: w=13, h=25. min_gx=-6 so pixel col = gx+6."""
    w, h = 13, 25
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 21:   p[(px, py)] = (12,  10,  16, 255)   # hood apex — near black
            elif py >= 17: p[(px, py)] = (18,  15,  22, 255)   # hood — very dark
            elif py >= 14: p[(px, py)] = (22,  18,  28, 255)   # cloaked shoulders
            elif py >= 10: p[(px, py)] = (20,  16,  25, 255)   # cloaked torso
            elif py >= 7:  p[(px, py)] = (25,  20,  30, 255)   # upper cloak bell
            elif py >= 4:  p[(px, py)] = (20,  15,  24, 255)   # mid cloak flare
            elif py >= 1:  p[(px, py)] = (16,  12,  20, 255)   # lower cloak
            else:          p[(px, py)] = (12,  10,  15, 255)   # tattered hem
    # Bone-white skull inside the hood (gx=-1..1 → col 5..7, gy=18-19 → py 18-19)
    for py in range(18, 20):
        for px in range(5, 8): p[(px, py)] = (215, 208, 200, 255)
    # Ghostly green-blue eye sockets
    p[(5, 19)] = (60, 220, 200, 255)   # left eye glow
    p[(7, 19)] = (60, 220, 200, 255)   # right eye glow
    # Broad shoulder drape — slightly lighter than body
    for py in range(14, 17):
        for px in range(2, 11): p[(px, py)] = (30, 25, 38, 255)
    # Shoulder edge caps
    for py in range(15, 17):
        p[(1, py)]  = (28, 22, 35, 255)
        p[(11, py)] = (28, 22, 35, 255)
    # Scythe pole — bone white (col 11, full height)
    for py in range(0, h):
        p[(11, py)] = (195, 188, 178, 255)   # pole — weathered bone
    # Scythe blade (top area, rows 22-23)
    for px in range(7, 12):
        p[(px, 22)] = (180, 180, 190, 255)   # blade face — steel grey
        p[(px, 21)] = (150, 148, 158, 255)   # blade lower edge
    p[(6, 22)] = (200, 200, 210, 255)         # blade tip highlight
    # Skeletal arm (left, col 2, rows 7-14)
    for py in range(7, 15):
        p[(2, py)] = (190, 182, 172, 255)    # left arm bone
    # Bony hands
    p[(1, 6)]  = (210, 202, 190, 255)        # left hand
    p[(10, 5)] = (210, 202, 190, 255)        # right hand (grips scythe)
    # Tattered hem tendrils — ghostly glow at floor
    for px in [1, 3, 5, 7, 9, 11]:
        p[(px, 0)] = (40, 150, 130, 255)     # ghostly green-teal hem
    return w, h, p


def skin_lich_lord():
    """Lich boss: tattered purple-grey robe, ivory skull, glowing violet eye-sockets.
    Grid matches gen_lich: w=9, h=20. min_gx=-4 so pixel col = gx+4."""
    w, h = 9, 20
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 17:   p[(px, py)] = (185, 170, 210, 255)   # hood/skull — ivory-grey
            elif py >= 13: p[(px, py)] = (80,  65, 130, 255)    # upper hood shadow/shoulders
            elif py >= 7:  p[(px, py)] = (90,  70, 150, 255)    # torso — muted purple robe
            elif py >= 4:  p[(px, py)] = (70,  55, 120, 255)    # upper skirt
            elif py >= 2:  p[(px, py)] = (60,  45, 105, 255)    # mid skirt (wider, darker)
            else:          p[(px, py)] = (50,  38,  90, 255)    # base skirt (darkest)
    # Crown spikes — gold
    for px in [0, 4, 8]:
        for py in range(18, 20):
            p[(px, py)] = (210, 185, 50, 255)
    # Skull face — ivory highlight
    for py in range(14, 17):
        for px in range(3, 6): p[(px, py)] = (215, 200, 180, 255)
    # Glowing violet eye-sockets (skull at gx=-1..1, gy=15 → pixel col 3,5 row 15)
    p[(3, 15)] = (220,  80, 255, 255)   # left eye glow
    p[(5, 15)] = (220,  80, 255, 255)   # right eye glow
    # Arms — slightly lighter bone
    for py in range(5, 13):
        p[(1, py)] = (160, 140, 200, 255)   # left arm column
        p[(7, py)] = (160, 140, 200, 255)   # right arm column
    # Raised right arm (col 7, upper rows) — brighter to show elevation
    for py in range(9, 13):
        p[(7, py)] = (190, 170, 230, 255)
    # Bony hands
    p[(1, 5)] = (200, 188, 160, 255)
    p[(7, 5)] = (200, 188, 160, 255)
    return w, h, p


def skin_spider_queen():
    """Spider Queen boss: mottled green-black carapace, swollen amber egg-sac, gold eye cluster.
    Grid matches gen_spider_queen: w=18, h=8. min_gx=-9 so pixel col = gx+9."""
    w, h = 18, 8
    p = {}
    carapace = (35, 60, 28, 255)    # dark green-black carapace base
    for py in range(h):
        for px in range(w):
            p[(px, py)] = carapace
    # Bloated egg-sac abdomen — swollen amber/translucent (rear, cols ~9-16 in pixel space)
    # Abdomen gx=-4..3 → pixel col 5..12; py rows 1..7 (gy=1..7)
    for py in range(1, 8):
        for px in range(5, 13):
            p[(px, py)] = (180, 130, 60, 255)   # amber translucent egg-sac
    # Abdomen dome highlight — lighter amber
    for px in range(6, 12):
        p[(px, 6)] = (220, 170, 90, 255)
        p[(px, 7)] = (200, 150, 75, 255)
    # Cephalothorax — mottled dark green (front center, gx=-3..2 → col 6..11, py 2..5)
    for py in range(2, 6):
        for px in range(6, 12):
            p[(px, py)] = (45, 75, 35, 255)
    # Head — darker chitin
    for py in range(2, 5):
        for px in range(7, 11):
            p[(px, py)] = (30, 50, 22, 255)
    # Eye crown cluster — gold (gy=5 → py=5, gx=-2..2 → col 7..11)
    p[(7, 5)]  = (255, 220, 40, 255)    # left eye stalk
    p[(9, 5)]  = (255, 220, 40, 255)    # center eye 1
    p[(10, 5)] = (255, 220, 40, 255)   # center eye 2
    p[(11, 5)] = (255, 220, 40, 255)   # right eye stalk
    # Fangs — pale bone
    p[(8, 0)]  = (210, 200, 170, 255)
    p[(10, 0)] = (210, 200, 170, 255)
    # Front-pair legs (raised higher, cols 6-9 pixel left and 9-12 right)
    for px in [4, 5]:
        for py in range(h): p[(px, py)] = (30, 50, 22, 255)   # inner front leg
    for px in [2, 3]:
        for py in range(h): p[(px, py)] = (25, 42, 18, 255)   # outer front leg
    for px in [0, 1]:
        for py in range(h): p[(px, py)] = (20, 35, 14, 255)   # tip front leg
    for px in [13, 14]:
        for py in range(h): p[(px, py)] = (30, 50, 22, 255)
    for px in [15, 16]:
        for py in range(h): p[(px, py)] = (25, 42, 18, 255)
    p[(17, py)] = (20, 35, 14, 255) if True else None
    for py in range(h): p[(17, py)] = (20, 35, 14, 255)
    # Leg joint highlights
    for (px, py) in [(1,3),(3,2),(4,4),(14,3),(16,2),(13,4)]:
        p[(px, py)] = (55, 90, 40, 255)
    return w, h, p


def skin_demon_knight():
    """Demon Knight/Azhar boss: ashen grey cracked stone with ember-orange glowing cracks.
    Grid matches gen_azhar: w=9, h=23. min_gx=-4 so pixel col = gx+4."""
    w, h = 9, 23
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 20:   p[(px, py)] = (130, 118, 110, 255)   # horns — light ashen
            elif py >= 15: p[(px, py)] = (100, 90, 82, 255)     # head — ashen grey
            elif py >= 12: p[(px, py)] = (88,  78, 70, 255)     # upper torso/shoulders
            elif py >= 7:  p[(px, py)] = (82,  72, 65, 255)     # main torso
            elif py >= 4:  p[(px, py)] = (70,  62, 56, 255)     # waist/cape
            elif py >= 2:  p[(px, py)] = (80,  70, 63, 255)     # thighs
            else:          p[(px, py)] = (60,  52, 48, 255)     # feet/claws
    # Ember-orange glowing cracks — scattered across body
    crack = (255, 120, 20, 255)
    p[(4, 15)] = crack; p[(3, 13)] = crack; p[(5, 11)] = crack
    p[(4, 9)]  = crack; p[(3, 7)]  = crack; p[(5, 5)]  = crack
    p[(2, 18)] = crack; p[(6, 16)] = crack   # face cracks
    # Yellow eyes (gx=-1,1 → col 3,5 at gy=18 → py 18)
    p[(3, 18)] = (255, 230, 50, 255)
    p[(5, 18)] = (255, 230, 50, 255)
    # Swept-back horn tips — lighter grey
    for py in range(20, 23):
        p[(2, py)] = (160, 148, 138, 255)    # left horn tip
        p[(6, py)] = (160, 148, 138, 255)    # right horn tip
    # Charcoal cape (thin back slab, col 4, rows 5-13)
    for py in range(5, 14):
        p[(4, py)] = (45, 38, 35, 255)       # cape center dark
    # Shoulder spikes — orange ember tips
    p[(0, 13)] = (255, 140, 30, 255)
    p[(8, 13)] = (255, 140, 30, 255)
    # Extended blade (right arm, col 7-8, rows 5-13)
    for py in range(5, 14):
        p[(7, py)] = (200, 195, 175, 255)    # blade — pale steel
    p[(7, 13)] = (255, 240, 160, 255)        # blade tip highlight
    # Clawed hand highlights
    p[(1, 5)]  = (210, 200, 175, 255)        # left claw
    p[(7, 4)]  = (210, 200, 175, 255)        # right claw/grip
    return w, h, p


def skin_arch_mage():
    """Arch Mage/Nyx boss: void black-purple robe, magenta crystal glow, bright pink eyes.
    Grid matches gen_nyx: w=9, h=23. min_gx=-4 so pixel col = gx+4."""
    w, h = 9, 23
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 19:   p[(px, py)] = (20,  12,  40, 255)    # hood — void black-purple
            elif py >= 15: p[(px, py)] = (28,  16,  55, 255)    # hooded head
            elif py >= 9:  p[(px, py)] = (35,  20,  65, 255)    # upper torso
            elif py >= 5:  p[(px, py)] = (25,  14,  50, 255)    # lower torso
            elif py >= 2:  p[(px, py)] = (20,  10,  40, 255)    # robe bell
            else:          p[(px, py)] = (15,   8,  32, 255)    # base tendrils
    # Void-crystal crown — magenta glow (top rows above hood)
    crown = (255, 60, 220, 255)
    for (px, py) in [(0,20),(1,21),(3,21),(4,22),(5,21),(7,21),(8,20)]:
        p[(px, py)] = crown
    p[(4, 22)] = (255, 120, 240, 255)   # tallest center crystal tip — bright pink
    # Face shadow cavity
    for py in range(16, 19):
        p[(4, py)] = (10, 5, 20, 255)   # center void-dark face cavity
    # Bright pink eyes (gx=-1,1 → col 3,5 at gy=17 → py=17)
    p[(3, 17)] = (255, 80, 255, 255)    # left eye bright pink glow
    p[(5, 17)] = (255, 80, 255, 255)    # right eye bright pink glow
    # Arms — slightly lighter void-purple
    for py in range(5, 14):
        p[(1, py)] = (42, 25, 80, 255)     # left arm
        p[(7, py)] = (42, 25, 80, 255)     # right arm
    # Hands — magenta glow at fingertips
    p[(0, 5)]  = (200, 50, 200, 255)   # left hand glow
    p[(8, 5)]  = (200, 50, 200, 255)   # right hand glow
    # Robe tendril tips — magenta
    for px in [0, 4, 8]:
        p[(px, 0)] = (180, 40, 175, 255)
    return w, h, p


def skin_hellhound():
    """Hellhound: fiery orange-red body, bright yellow eyes, white fangs.
    Grid: x=[-3,2] (w=6), y=[0,9] (h=10). min_x=-3, min_y=0.
    px=gx+3, py=gy."""
    w, h = 6, 10
    p = {}
    body = (100, 40, 15, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body

    # Body — gradient, brighter on back half (upper py)
    for py in range(3, 8):
        for px in range(1, 5): p[(px, py)] = (150, 55, 18, 255)
    for px in range(2, 4): p[(px, 7)] = (180, 70, 25, 255)
    # Head area (upper body py 6-8)
    for py in range(6, 9):
        for px in range(2, 4): p[(px, py)] = (170, 68, 22, 255)
    # Eyes — bright yellow (near top, py 9)
    p[(2, 9)] = (240, 220, 40, 255)
    p[(3, 9)] = (240, 220, 40, 255)
    # Fangs — white (bottom py 0)
    p[(2, 0)] = (200, 200, 180, 255)
    p[(3, 0)] = (200, 200, 180, 255)
    # Legs — dark charred (outer columns)
    for py in range(h):
        p[(0, py)] = (80, 30, 10, 255)
        p[(5, py)] = (80, 30, 10, 255)
    # Leg accents
    for (px, py) in [(0, 3), (0, 5), (5, 3), (5, 5)]:
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
    """Human-mesh zombie: vivid sickly green flesh, bright red eyes.
    Grid: x=[-4,4] (w=9), y=[0,15] (h=16). py=gy, px=gx+4."""
    w, h = 9, 16
    p = {}
    # Fill by body region matching human mesh proportions (zombie colors)
    for py in range(h):
        for px in range(w):
            if py >= 13:   p[(px, py)] = (80, 165, 70, 255)    # green flesh (face/head, gy 13-15)
            elif py >= 11: p[(px, py)] = (70, 150, 60, 255)    # neck (gy 11-12)
            elif py >= 5:  p[(px, py)] = (55, 130, 50, 255)    # torso (tattered rags, gy 5-10)
            elif py >= 4:  p[(px, py)] = (45, 100, 40, 255)    # belt/waist (gy 4)
            elif py >= 2:  p[(px, py)] = (50, 115, 45, 255)    # legs (gy 2-3)
            else:          p[(px, py)] = (40, 70, 30, 255)     # muddy feet (gy 0-1)
    # Exposed brain on top-right of skull (gy 15 → py 15, gy 14 area)
    # Right side of head (px 5-6) shows brain through cracked skull
    p[(5, 15)] = (200, 80, 90, 255)    # brain on top
    p[(6, 15)] = (190, 70, 80, 255)
    p[(5, 14)] = (210, 85, 95, 255)    # brain visible at skull level
    p[(6, 14)] = (195, 75, 85, 255)
    # Left side stays dark stringy hair (top py 15)
    for px in range(1, 5): p[(px, 15)] = (50, 70, 35, 255)
    # Eyes — bright glowing red. Human mesh: gx=-1 → px=3, gx=+1 → px=5 (but brain covers px5,py14)
    # Use px=3 and px=5; restore eye over brain position
    p[(3, 14)] = (240, 40, 20, 255)
    p[(5, 14)] = (240, 40, 20, 255)
    return w, h, p


def skin_ghoul():
    """Skeleton-rig ghoul: pale sickly green flesh, bright yellow predator eyes.
    Grid: x=[-3,3] (w=7), y=[0,15] (h=16). py=gy directly."""
    w, h = 7, 16
    p = {}
    base = (130, 150, 100, 255)  # pale sickly green
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Skull/face — slightly lighter (gy 14-15 → py 14-15)
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (145, 165, 115, 255)
    # Eyes — bright yellow (gy 14 → py 14)
    p[(2, 14)] = (220, 200, 30, 255)
    p[(4, 14)] = (220, 200, 30, 255)
    # Nose socket (gy 13 → py 13)
    p[(3, 13)] = (100, 115, 75, 255)
    # Jaw (gy 11 → py 11)
    p[(2, 11)] = (150, 165, 120, 255)  # pale teeth
    p[(3, 11)] = (70, 60, 45, 255)    # mouth gap
    p[(4, 11)] = (150, 165, 120, 255)
    # Skull sides (gy 13 → py 13)
    p[(1, 13)] = (120, 138, 92, 255)
    p[(5, 13)] = (120, 138, 92, 255)
    # Spine (gy 5-10 → py 5-10)
    for py in range(5, 11): p[(3, py)] = (110, 128, 85, 255)
    # Ribs — gaunt gray-green torso (gy 6,7,9 → py 6,7,9)
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (100, 120, 75, 255)
    # Rib gaps (gy 8 → py 8)
    for px in [1, 2, 4, 5]: p[(px, 8)] = (65, 75, 48, 255)
    # Shoulders (gy 9 → py 9)
    p[(0, 9)] = (115, 135, 90, 255)
    p[(6, 9)] = (115, 135, 90, 255)
    # Arms (gy 3-8 → py 3-8)
    for py in range(3, 9):
        p[(0, py)] = (120, 140, 95, 255)
        p[(6, py)] = (120, 140, 95, 255)
    # Pelvis (gy 4 → py 4)
    for px in range(2, 5): p[(px, 4)] = (110, 128, 85, 255)
    # Legs (gy 0-3 → py 0-3)
    for px in [1, 4]:
        p[(px, 2)] = base; p[(px, 3)] = base
        p[(px, 0)] = (105, 122, 80, 255); p[(px, 1)] = (108, 125, 82, 255)
    return w, h, p


def skin_bone_mage():
    """Skeleton-rig bone mage: purple-white bone, glowing purple eyes, dark robe over torso/legs.
    Grid: x=[-3,3] (w=7), y=[0,15] (h=16). py=gy directly."""
    w, h = 7, 16
    p = {}
    base = (170, 160, 190, 255)  # purple-white bone
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Skull — pale lavender (gy 14-15 → py 14-15)
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (180, 175, 200, 255)
    # Eyes — glowing purple (gy 14 → py 14)
    p[(2, 14)] = (160, 60, 220, 255)
    p[(4, 14)] = (160, 60, 220, 255)
    # Nose socket (gy 13 → py 13)
    p[(3, 13)] = (130, 120, 150, 255)
    # Jaw (gy 11 → py 11)
    p[(2, 11)] = (185, 180, 205, 255)  # pale lavender teeth
    p[(3, 11)] = (90, 70, 110, 255)    # dark mouth gap
    p[(4, 11)] = (185, 180, 205, 255)
    # Skull sides (gy 13 → py 13)
    p[(1, 13)] = (155, 148, 172, 255)
    p[(5, 13)] = (155, 148, 172, 255)
    # Spine — dark purple robe column (gy 5-10 → py 5-10)
    for py in range(5, 11): p[(3, py)] = (100, 78, 135, 255)
    # Torso — dark purple robe covering ribs (gy 5-9 → py 5-9)
    for py in range(5, 10):
        for px in [1, 2, 4, 5]: p[(px, py)] = (80, 50, 120, 255)
    # Rib gaps hidden under robe (gy 8 → py 8)
    for px in [1, 2, 4, 5]: p[(px, 8)] = (70, 45, 105, 255)
    # Shoulders (gy 9 → py 9)
    p[(0, 9)] = (140, 132, 158, 255)
    p[(6, 9)] = (140, 132, 158, 255)
    # Arms — bone visible beneath robe sleeves (gy 3-8 → py 3-8)
    for py in range(3, 9):
        p[(0, py)] = (158, 150, 175, 255)
        p[(6, py)] = (158, 150, 175, 255)
    # Pelvis — robe continues (gy 4 → py 4)
    for px in range(2, 5): p[(px, 4)] = (75, 48, 108, 255)
    # Legs — robe lower portion (gy 0-3 → py 0-3)
    for px in [1, 4]:
        p[(px, 2)] = (70, 45, 100, 255); p[(px, 3)] = (70, 45, 100, 255)
        p[(px, 0)] = (155, 148, 172, 255); p[(px, 1)] = (145, 138, 162, 255)  # bone feet
    return w, h, p


def skin_stalker():
    """Skeleton-rig stalker (Cavern Stalker): dark charcoal undead assassin, amber hunter eyes.
    Grid: x=[-3,3] (w=7), y=[0,15] (h=16). py=gy directly."""
    w, h = 7, 16
    p = {}
    base = (75, 65, 100, 255)  # lighter purple-gray (contrasts cavern walls)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Face — slightly lighter charcoal (gy 14-15 → py 14-15)
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (70, 65, 80, 255)
    # Eyes — amber (gy 14 → py 14)
    p[(2, 14)] = (200, 160, 40, 255)
    p[(4, 14)] = (200, 160, 40, 255)
    # Nose socket (gy 13 → py 13)
    p[(3, 13)] = (42, 38, 50, 255)
    # Jaw (gy 11 → py 11)
    p[(2, 11)] = (72, 68, 82, 255)    # darker teeth
    p[(3, 11)] = (30, 25, 35, 255)    # dark mouth
    p[(4, 11)] = (72, 68, 82, 255)
    # Skull sides (gy 13 → py 13)
    p[(1, 13)] = (60, 56, 70, 255)
    p[(5, 13)] = (60, 56, 70, 255)
    # Spine (gy 5-10 → py 5-10)
    for py in range(5, 11): p[(3, py)] = (45, 40, 55, 255)
    # Ribs — sleek dark torso (gy 6,7,9 → py 6,7,9)
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (50, 45, 60, 255)
    # Rib gaps (gy 8 → py 8)
    for px in [1, 2, 4, 5]: p[(px, 8)] = (28, 24, 32, 255)
    # Shoulders (gy 9 → py 9)
    p[(0, 9)] = (48, 44, 58, 255)
    p[(6, 9)] = (48, 44, 58, 255)
    # Arms (gy 3-8 → py 3-8)
    for py in range(3, 9):
        p[(0, py)] = (50, 46, 60, 255)
        p[(6, py)] = (50, 46, 60, 255)
    # Pelvis (gy 4 → py 4)
    for px in range(2, 5): p[(px, 4)] = (46, 42, 56, 255)
    # Legs (gy 0-3 → py 0-3)
    for px in [1, 4]:
        p[(px, 2)] = base; p[(px, 3)] = base
        p[(px, 0)] = (40, 36, 52, 255); p[(px, 1)] = (42, 38, 54, 255)
    return w, h, p


def skin_demon():
    """Skeleton-rig demon (Demon Caster, Hellforged Reaver): deep crimson bone, orange-yellow ember eyes, black rib gaps.
    Grid: x=[-3,3] (w=7), y=[0,15] (h=16). py=gy directly."""
    w, h = 7, 16
    p = {}
    base = (140, 35, 25, 255)  # deep crimson
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Face/skull — dark red (gy 14-15 → py 14-15)
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (120, 25, 20, 255)
    # Eyes — orange-yellow ember (gy 14 → py 14)
    p[(2, 14)] = (230, 180, 30, 255)
    p[(4, 14)] = (230, 180, 30, 255)
    # Nose socket (gy 13 → py 13)
    p[(3, 13)] = (90, 15, 12, 255)
    # Jaw (gy 11 → py 11)
    p[(2, 11)] = (130, 30, 22, 255)   # dark crimson teeth
    p[(3, 11)] = (50, 8, 5, 255)      # near-black mouth gap
    p[(4, 11)] = (130, 30, 22, 255)
    # Skull sides (gy 13 → py 13)
    p[(1, 13)] = (110, 22, 18, 255)
    p[(5, 13)] = (110, 22, 18, 255)
    # Spine (gy 5-10 → py 5-10)
    for py in range(5, 11): p[(3, py)] = (90, 18, 12, 255)
    # Ribs — dark red torso (gy 6,7,9 → py 6,7,9)
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (100, 20, 15, 255)
    # Rib gaps — very dark ember void (gy 8 → py 8)
    for px in [1, 2, 4, 5]: p[(px, 8)] = (40, 6, 4, 255)
    # Shoulders (gy 9 → py 9)
    p[(0, 9)] = (120, 28, 20, 255)
    p[(6, 9)] = (120, 28, 20, 255)
    # Arms (gy 3-8 → py 3-8)
    for py in range(3, 9):
        p[(0, py)] = (125, 30, 22, 255)
        p[(6, py)] = (125, 30, 22, 255)
    # Pelvis (gy 4 → py 4)
    for px in range(2, 5): p[(px, 4)] = (110, 22, 16, 255)
    # Legs (gy 0-3 → py 0-3)
    for px in [1, 4]:
        p[(px, 2)] = base; p[(px, 3)] = base
        p[(px, 0)] = (105, 20, 14, 255); p[(px, 1)] = (108, 21, 15, 255)
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
    """Skeleton-rig void demon (Void Stalker): dark indigo bone, bright purple void-glow eyes.
    Grid: x=[-3,3] (w=7), y=[0,15] (h=16). py=gy directly."""
    w, h = 7, 16
    p = {}
    base = (50, 35, 80, 255)  # dark indigo
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Face — indigo (gy 14-15 → py 14-15)
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (60, 45, 90, 255)
    # Eyes — bright purple void glow (gy 14 → py 14)
    p[(2, 14)] = (180, 80, 255, 255)
    p[(4, 14)] = (180, 80, 255, 255)
    # Nose socket (gy 13 → py 13)
    p[(3, 13)] = (38, 26, 62, 255)
    # Jaw (gy 11 → py 11)
    p[(2, 11)] = (62, 48, 92, 255)    # dark indigo teeth
    p[(3, 11)] = (22, 14, 40, 255)    # void mouth gap
    p[(4, 11)] = (62, 48, 92, 255)
    # Skull sides (gy 13 → py 13)
    p[(1, 13)] = (52, 38, 78, 255)
    p[(5, 13)] = (52, 38, 78, 255)
    # Spine (gy 5-10 → py 5-10)
    for py in range(5, 11): p[(3, py)] = (38, 26, 62, 255)
    # Ribs — deep dark indigo torso (gy 6,7,9 → py 6,7,9)
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (40, 28, 65, 255)
    # Rib gaps (gy 8 → py 8)
    for px in [1, 2, 4, 5]: p[(px, 8)] = (20, 12, 35, 255)
    # Shoulders (gy 9 → py 9)
    p[(0, 9)] = (44, 30, 70, 255)
    p[(6, 9)] = (44, 30, 70, 255)
    # Arms (gy 3-8 → py 3-8)
    for py in range(3, 9):
        p[(0, py)] = (46, 32, 72, 255)
        p[(6, py)] = (46, 32, 72, 255)
    # Pelvis (gy 4 → py 4)
    for px in range(2, 5): p[(px, 4)] = (40, 28, 65, 255)
    # Legs (gy 0-3 → py 0-3)
    for px in [1, 4]:
        p[(px, 2)] = base; p[(px, 3)] = base
        p[(px, 0)] = (35, 22, 58, 255); p[(px, 1)] = (38, 25, 62, 255)
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


# ---------------------------------------------------------------------------
# New archetype enemy skins (w=7, h=16 skeleton rig)
# ---------------------------------------------------------------------------

def skin_gargoyle():
    """Gargoyle — grey stone body, amber eyes, mossy green patches.
    Grid: x=[-4,4] (w=9), y=[0,13] (h=14). min_x=-4, min_y=0.
    px=gx+4, py=gy."""
    w, h = 9, 14
    p = {}
    stone = (150, 150, 155, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = stone
    # Head — lighter stone (top 2 rows: py 12-13)
    for py in [12, 13]:
        for px in range(2, 7): p[(px, py)] = (170, 170, 175, 255)
    # Amber eyes (py 12, center-left/right of head)
    p[(3, 12)] = (200, 150, 40, 255)
    p[(5, 12)] = (200, 150, 40, 255)
    # Mossy patches on torso (mid-body)
    for px in [3, 5]:
        p[(px, 8)] = (80, 110, 60, 255)
        p[(px, 9)] = (80, 110, 60, 255)
    # Wing plates — darker stone (outer columns, mid-body)
    for py in range(7, 11):
        p[(0, py)] = (110, 110, 115, 255)
        p[(1, py)] = (115, 115, 120, 255)
        p[(7, py)] = (115, 115, 120, 255)
        p[(8, py)] = (110, 110, 115, 255)
    # Claws — dark (lower outer columns)
    p[(0, 3)] = (80, 80, 85, 255)
    p[(8, 3)] = (80, 80, 85, 255)
    # Legs — slightly darker (lower body)
    for py in range(0, 5):
        for px in [2, 6]: p[(px, py)] = (130, 130, 135, 255)
    return w, h, p


def skin_necromancer():
    """Necromancer — dark purple bones, green eyes, black robes.
    Grid: x=[-4,4] (w=9), y=[0,17] (h=18). min_x=-4, min_y=0.
    px=gx+4, py=gy."""
    w, h = 9, 18
    p = {}
    robe = (25, 20, 30, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = robe
    # Hood — dark purple (top rows py 16-17)
    for py in [16, 17]:
        for px in range(2, 7): p[(px, py)] = (60, 30, 80, 255)
    # Green glowing eyes (py 16)
    p[(3, 16)] = (40, 200, 40, 255)
    p[(5, 16)] = (40, 200, 40, 255)
    # Skull visible under hood (py 15)
    p[(4, 15)] = (80, 40, 120, 255)
    # Purple bone hands (mid-height py 8, outer columns)
    p[(0, 8)] = (80, 40, 120, 255)
    p[(8, 8)] = (80, 40, 120, 255)
    # Robe hem — slightly lighter (bottom rows)
    for px in range(w):
        p[(px, 0)] = (35, 28, 40, 255)
        p[(px, 1)] = (30, 24, 36, 255)
    # Robe trim — faint purple (proportional to h=18: ~py 5 and 10)
    for py in [5, 10]:
        for px in range(w): p[(px, py)] = (40, 25, 50, 255)
    return w, h, p


def skin_cavern_shaman():
    """Cavern Shaman — green-brown body, bone-white headdress, blue hands."""
    w, h = 7, 16
    p = {}
    body = (90, 100, 60, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body
    # Horned headdress — bone white
    for py in [14, 15]:
        for px in range(w): p[(px, py)] = (220, 210, 200, 255)
    # Eyes — warm amber
    p[(2, 13)] = (180, 140, 40, 255)
    p[(4, 13)] = (180, 140, 40, 255)
    # Face — darker
    for px in range(1, 6): p[(px, 12)] = (70, 80, 45, 255)
    # Blue glowing hands
    p[(0, 6)] = (60, 120, 200, 255)
    p[(6, 6)] = (60, 120, 200, 255)
    p[(0, 5)] = (60, 120, 200, 255)
    p[(6, 5)] = (60, 120, 200, 255)
    # Shoulders — darker green
    for px in [0, 6]:
        p[(px, 9)] = (70, 85, 45, 255)
        p[(px, 10)] = (70, 85, 45, 255)
    # Belt
    for px in range(w): p[(px, 5)] = (100, 75, 40, 255)
    # Legs — earthen brown
    for py in range(0, 4):
        for px in range(w): p[(px, py)] = (80, 70, 45, 255)
    return w, h, p


def skin_cavern_herald():
    """Cavern Herald — earthy green-brown body, glowing amber core, amber eyes.
    Grid: x=[-5,5] (w=11), y=[0,19] (h=20). min_x=-5, min_y=0.
    px=gx+5, py=gy."""
    w, h = 11, 20
    p = {}
    body = (80, 95, 55, 255)  # mossy green-brown
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body
    # Skull — bone with green tinge (top py 18-19)
    for py in [18, 19]:
        for px in range(3, 8): p[(px, py)] = (160, 155, 120, 255)
    # Amber glowing eyes (py 18)
    p[(4, 18)] = (220, 170, 30, 255)
    p[(6, 18)] = (220, 170, 30, 255)
    # Amber aura core (chest py 13-15)
    for py in [13, 14, 15]:
        p[(5, py)] = (200, 160, 40, 255)
    p[(4, 14)] = (180, 140, 30, 255)
    p[(6, 14)] = (180, 140, 30, 255)
    # Spine — darker (py 10-16)
    for py in range(10, 17): p[(5, py)] = (60, 75, 40, 255)
    # Arms — darker green (outer columns py 7-14)
    for py in range(7, 15):
        p[(0, py)] = (65, 80, 40, 255)
        p[(10, py)] = (65, 80, 40, 255)
    # Shoulders (py 12-13)
    for px in [0, 10]:
        p[(px, 12)] = (90, 85, 55, 255)
        p[(px, 13)] = (90, 85, 55, 255)
    # Legs — earthen brown (lower py 0-6)
    for py in range(0, 7):
        for px in [2, 8]: p[(px, py)] = (70, 65, 40, 255)
    return w, h, p


def skin_crypt_herald():
    """Crypt Herald (tier 2) — sickly green bone, toxic green aura core, green eyes.
    Grid: x=[-5,5] (w=11), y=[0,19] (h=20). min_x=-5, min_y=0.
    px=gx+5, py=gy."""
    w, h = 11, 20
    p = {}
    bone = (120, 140, 90, 255)  # sickly green-tan bone
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bone
    # Skull — pale green bone (top py 18-19)
    for py in [18, 19]:
        for px in range(3, 8): p[(px, py)] = (150, 165, 115, 255)
    # Toxic green glowing eyes (py 18)
    p[(4, 18)] = (80, 220, 60, 255)
    p[(6, 18)] = (80, 220, 60, 255)
    # Green aura core (chest) — toxic glow (py 13-15)
    for py in [13, 14, 15]:
        p[(5, py)] = (60, 200, 50, 255)
    p[(4, 14)] = (50, 180, 40, 255)
    p[(6, 14)] = (50, 180, 40, 255)
    # Spine — darker (py 10-16)
    for py in range(10, 17): p[(5, py)] = (80, 100, 55, 255)
    # Arms — dark green (outer columns, mid-body py 7-14)
    for py in range(7, 15):
        p[(0, py)] = (90, 110, 60, 255)
        p[(10, py)] = (90, 110, 60, 255)
    # Shoulders (py 12-13)
    for px in [0, 10]:
        p[(px, 12)] = (110, 125, 75, 255)
        p[(px, 13)] = (110, 125, 75, 255)
    # Legs — dark mossy (lower py 0-6)
    for py in range(0, 7):
        for px in [2, 8]: p[(px, py)] = (75, 90, 50, 255)
    return w, h, p


def skin_sniper_imp():
    """Sniper Imp (tier 3) — dark purple body, bright yellow hawk eyes, crystal-tipped wings."""
    w, h = 5, 10
    p = {}
    body = (70, 50, 90, 255)  # dark purple
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body
    # Belly — slightly lighter
    for py in range(3, 7):
        for px in range(1, 4): p[(px, py)] = (85, 65, 105, 255)
    # Sharp yellow hawk eyes — same position as base bat (py=6)
    p[(1, 6)] = (240, 220, 40, 255)
    p[(3, 6)] = (240, 220, 40, 255)
    # Wing tips — crystal blue (targeting crystals)
    p[(0, 7)] = (120, 160, 220, 255)
    p[(4, 7)] = (120, 160, 220, 255)
    p[(0, 8)] = (100, 140, 200, 255)
    p[(4, 8)] = (100, 140, 200, 255)
    # Darker wing base
    for py in range(4, 7):
        p[(0, py)] = (55, 40, 75, 255)
        p[(4, py)] = (55, 40, 75, 255)
    # Feet — dark claws
    p[(1, 0)] = (45, 30, 55, 255)
    p[(3, 0)] = (45, 30, 55, 255)
    return w, h, p


def skin_infernal_herald():
    """Infernal Herald — orange-red bones, fiery core, ember eyes.
    Grid: x=[-5,5] (w=11), y=[0,19] (h=20). min_x=-5, min_y=0.
    px=gx+5, py=gy."""
    w, h = 11, 20
    p = {}
    bone = (200, 80, 30, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bone
    # Skull (top py 18-19)
    for py in [18, 19]:
        for px in range(3, 8): p[(px, py)] = (220, 100, 40, 255)
    # Ember eyes (py 18)
    p[(4, 18)] = (255, 120, 20, 255)
    p[(6, 18)] = (255, 120, 20, 255)
    # Fiery core (open chest py 13-15)
    for py in [13, 14, 15]:
        p[(5, py)] = (255, 180, 40, 255)
    p[(4, 14)] = (255, 200, 60, 255)
    p[(6, 14)] = (255, 200, 60, 255)
    # Spine (py 10-16)
    for py in range(10, 17): p[(5, py)] = (160, 60, 20, 255)
    # Arms — darker red (outer columns py 7-14)
    for py in range(7, 15):
        p[(0, py)] = (160, 60, 20, 255)
        p[(10, py)] = (160, 60, 20, 255)
    # Legs — cooling ember (lower py 0-6)
    for py in range(0, 7):
        for px in [2, 8]: p[(px, py)] = (140, 50, 15, 255)
    return w, h, p


def skin_void_necromancer():
    """Void Necromancer — dark purple-void robes, ice-blue eyes.
    Grid: x=[-4,4] (w=9), y=[0,17] (h=18). min_x=-4, min_y=0.
    px=gx+4, py=gy."""
    w, h = 9, 18
    p = {}
    robe = (35, 20, 55, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = robe
    # Hood (top rows py 16-17)
    for py in [16, 17]:
        for px in range(2, 7): p[(px, py)] = (60, 30, 90, 255)
    # Ice-blue eyes (py 16)
    p[(3, 16)] = (80, 160, 255, 255)
    p[(5, 16)] = (80, 160, 255, 255)
    # Skull visible under hood (py 15)
    p[(4, 15)] = (50, 25, 75, 255)
    # Robe hem (py 0)
    for px in range(w):
        p[(px, 0)] = (45, 28, 65, 255)
    # Robe trim (proportional: py 5 and 10)
    for py in [5, 10]:
        for px in range(w): p[(px, py)] = (50, 30, 70, 255)
    # Purple void bone hands (py 8, outer columns)
    p[(0, 8)] = (60, 30, 90, 255)
    p[(8, 8)] = (60, 30, 90, 255)
    return w, h, p


def skin_void_shaman():
    """Void Shaman — teal void body, white headdress."""
    w, h = 7, 16
    p = {}
    body = (40, 100, 100, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body
    for py in [14, 15]:
        for px in range(w): p[(px, py)] = (180, 180, 200, 255)
    p[(2, 13)] = (100, 200, 255, 255)
    p[(4, 13)] = (100, 200, 255, 255)
    for px in range(1, 6): p[(px, 12)] = (30, 80, 80, 255)
    p[(0, 6)] = (60, 160, 200, 255)
    p[(6, 6)] = (60, 160, 200, 255)
    for px in range(w): p[(px, 5)] = (50, 80, 80, 255)
    for py in range(0, 4):
        for px in range(w): p[(px, py)] = (30, 75, 75, 255)
    return w, h, p


def skin_void_herald():
    """Void Herald — black void body, blue glowing core.
    Grid: x=[-5,5] (w=11), y=[0,19] (h=20). min_x=-5, min_y=0.
    px=gx+5, py=gy."""
    w, h = 11, 20
    p = {}
    bone = (25, 25, 35, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bone
    # Skull (top py 18-19)
    for py in [18, 19]:
        for px in range(3, 8): p[(px, py)] = (35, 35, 50, 255)
    # Blue glowing eyes (py 18)
    p[(4, 18)] = (80, 160, 255, 255)
    p[(6, 18)] = (80, 160, 255, 255)
    # Blue glowing core (chest py 13-15)
    for py in [13, 14, 15]:
        p[(5, py)] = (60, 120, 255, 255)
    p[(4, 14)] = (80, 150, 255, 255)
    p[(6, 14)] = (80, 150, 255, 255)
    # Spine — near-black (py 10-16)
    for py in range(10, 17): p[(5, py)] = (20, 20, 30, 255)
    # Arms — very dark (outer columns py 7-14)
    for py in range(7, 15):
        p[(0, py)] = (20, 20, 30, 255)
        p[(10, py)] = (20, 20, 30, 255)
    # Legs — darkest (lower py 0-6)
    for py in range(0, 7):
        for px in [2, 8]: p[(px, py)] = (18, 18, 28, 255)
    return w, h, p


# ---------------------------------------------------------------------------
# Additional enemy skins — mixed rigs
# ---------------------------------------------------------------------------

def skin_bone_archer():
    """Skeleton-rig bone archer: pale bone white body, dark leather straps, ice-blue eyes.
    Grid: x=[-3,3] (w=7), y=[0,15] (h=16). py=gy directly."""
    w, h = 7, 16
    p = {}
    bone = (230, 220, 200, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bone

    # Darker bone at extremities (feet py 0-1 = gy 0-1, hands py 3-8)
    for py in range(0, 2):
        for px in range(w): p[(px, py)] = (200, 190, 170, 255)
    for py in range(3, 9):
        p[(0, py)] = (200, 190, 170, 255)
        p[(6, py)] = (200, 190, 170, 255)
    # Skull (gy 14-15 → py 14-15) — same bone color as body, only eyes stand out
    # Ice-blue eyes (gy 14 → py 14)
    p[(2, 14)] = (100, 180, 240, 255)
    p[(4, 14)] = (100, 180, 240, 255)
    # Nose socket (gy 13 → py 13)
    p[(3, 13)] = (180, 170, 150, 255)
    # Dark brown leather straps across torso (gy 7-9 → py 7-9)
    for py in range(7, 10):
        for px in range(2, 5): p[(px, py)] = (70, 50, 30, 255)
    # Quiver strap diagonal on back (gy 7-10 → py 7-10)
    p[(2, 10)] = (65, 45, 28, 255)
    p[(3, 9)] = (65, 45, 28, 255)
    p[(4, 8)] = (65, 45, 28, 255)
    p[(5, 7)] = (65, 45, 28, 255)
    # Spine (gy 5-9 → py 5-9)
    for py in range(5, 10): p[(3, py)] = (195, 185, 165, 255)
    # Pelvis (gy 4 → py 4)
    for px in range(2, 5): p[(px, 4)] = (190, 180, 160, 255)
    # Legs (gy 0-3 → py 0-3)
    for px in [1, 4]:
        p[(px, 2)] = bone; p[(px, 3)] = bone
        p[(px, 0)] = (200, 190, 170, 255); p[(px, 1)] = (205, 195, 175, 255)
    return w, h, p


def skin_catacomb_sentinel():
    """Skeleton-rig catacomb sentinel: iron-grey plated body, toxic green eyes."""
    w, h = 7, 16
    p = {}
    body = (140, 145, 150, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body

    # Darker grey plating on torso
    for py in range(6, 11):
        for px in range(1, 6): p[(px, py)] = (110, 115, 120, 255)
    # Bright metal shield-arm highlight on left
    for py in range(5, 10):
        p[(0, py)] = (180, 185, 195, 255)
    # Skull
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (155, 160, 165, 255)
    # Toxic green eyes
    p[(2, 14)] = (80, 220, 60, 255)
    p[(4, 14)] = (80, 220, 60, 255)
    # Greenish tint on bone areas
    for py in range(11, 14):
        for px in range(1, 6): p[(px, py)] = (135, 150, 140, 255)
    # Spine
    for py in range(5, 10): p[(3, py)] = (100, 105, 110, 255)
    # Pelvis
    for px in range(2, 5): p[(px, 4)] = (120, 125, 130, 255)
    # Arms
    for py in range(3, 9):
        p[(0, py)] = (130, 135, 140, 255)
        p[(6, py)] = (130, 135, 140, 255)
    # Legs — slightly darker
    for px in [1, 4]:
        p[(px, 0)] = (120, 125, 130, 255); p[(px, 1)] = (125, 130, 135, 255)
        p[(px, 2)] = body; p[(px, 3)] = body
    return w, h, p


def skin_tomb_wraith():
    """Tomb wraith: semi-transparent blue-white ethereal body, ice-blue eyes.
    Grid: x=[-2,2] (w=5), y=[0,17] (h=18). min_x=-2, min_y=0.
    px=gx+2, py=gy."""
    w, h = 5, 18
    p = {}
    # Low alpha for ghostly appearance
    ghost = (180, 200, 220, 180)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = ghost

    # Brighter glow at center torso (mid-body py 7-12)
    for py in range(7, 13):
        for px in range(1, 4): p[(px, py)] = (210, 225, 240, 200)
    # Skull — faint glow (top py 16-17)
    for py in [16, 17]:
        for px in range(1, 4): p[(px, py)] = (200, 215, 230, 190)
    # Ice-blue eyes (py 16)
    p[(1, 16)] = (150, 200, 255, 220)
    p[(3, 16)] = (150, 200, 255, 220)
    # Ethereal wisps along center column
    for py in range(5, 14): p[(2, py)] = (170, 190, 210, 160)
    # Arms — fading (outer columns, mid-body)
    for py in range(4, 13):
        p[(0, py)] = (160, 180, 200, 150)
        p[(4, py)] = (160, 180, 200, 150)
    # Feet — almost invisible (bottom py 0-1)
    for py in range(0, 2):
        for px in range(w): p[(px, py)] = (170, 190, 210, 120)
    return w, h, p


def skin_plague_bat():
    """Bat-rig plague bat: sickly yellow-green body, pustule marks, infected red eyes."""
    w, h = 5, 10
    p = {}
    body = (140, 160, 60, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body

    # Dark green wing edges
    for py in range(h):
        p[(0, py)] = (90, 110, 40, 255)
        p[(4, py)] = (90, 110, 40, 255)
    # Lighter belly
    for py in range(0, 4):
        for px in range(1, 4): p[(px, py)] = (160, 175, 70, 255)
    # Pustule marks — bright green dots
    p[(1, 5)] = (100, 200, 40, 255)
    p[(3, 7)] = (100, 200, 40, 255)
    p[(2, 3)] = (100, 200, 40, 255)
    # Infected red eyes — same position as base bat (py=6)
    p[(1, 6)] = (220, 50, 30, 255)
    p[(3, 6)] = (220, 50, 30, 255)
    # Ears
    p[(1, 8)] = (120, 140, 50, 255)
    p[(3, 8)] = (120, 140, 50, 255)
    # Snout
    p[(2, 4)] = (150, 170, 65, 255)
    p[(2, 5)] = (130, 150, 55, 255)
    # Claws
    for px in [1, 3]:
        p[(px, 0)] = (80, 100, 35, 255)
        p[(px, 1)] = (80, 100, 35, 255)
    # Wing shoulders
    p[(0, 2)] = (100, 120, 45, 255)
    p[(4, 2)] = (100, 120, 45, 255)
    return w, h, p


def skin_web_spinner():
    """Spider-rig web spinner: light silvery grey body, dark web-pattern lines, pale blue eyes."""
    w, h = 15, 7
    p = {}
    body = (170, 170, 175, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body

    # Abdomen
    for py in range(1, 6):
        for px in range(4, 10): p[(px, py)] = (175, 175, 180, 255)
    # Dark grey web-pattern lines on abdomen
    for py in [2, 4]:
        for px in [5, 7, 9]: p[(px, py)] = (100, 100, 110, 255)
    for px in [4, 6, 8]:
        p[(px, 3)] = (100, 100, 110, 255)
    # Silk-white spinnerets
    for px in range(5, 9): p[(px, 0)] = (220, 220, 225, 255)
    # Head
    for py in range(1, 4):
        for px in range(6, 9): p[(px, py)] = (165, 165, 170, 255)
    # Pale blue eyes
    p[(6, 6)] = (150, 180, 220, 255)
    p[(8, 6)] = (150, 180, 220, 255)
    # Fangs
    p[(6, 0)] = (200, 200, 205, 255)
    p[(8, 0)] = (200, 200, 205, 255)
    # Legs — slightly darker grey
    for px in [0, 1]:
        for py in range(h): p[(px, py)] = (140, 140, 148, 255)
    for px in [2, 3]:
        for py in range(h): p[(px, py)] = (155, 155, 162, 255)
    for px in [13, 14]:
        for py in range(h): p[(px, py)] = (140, 140, 148, 255)
    for px in [10, 11, 12]:
        for py in range(h): p[(px, py)] = (155, 155, 162, 255)
    return w, h, p


def skin_burrowing_widow():
    """Spider-rig burrowing widow: dark earthy brown-black body, red hourglass, orange eyes."""
    w, h = 15, 7
    p = {}
    body = (45, 35, 30, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body

    # Abdomen
    for py in range(1, 6):
        for px in range(4, 10): p[(px, py)] = (50, 40, 34, 255)
    # Pale underbelly (py 0-1 center lighter)
    for py in range(0, 2):
        for px in range(5, 9): p[(px, py)] = (80, 65, 55, 255)
    # Red hourglass on abdomen
    p[(6, 3)] = (200, 30, 20, 255)
    p[(7, 3)] = (200, 30, 20, 255)
    # Head
    for py in range(1, 4):
        for px in range(6, 9): p[(px, py)] = (48, 38, 32, 255)
    # Bright orange eyes
    p[(6, 6)] = (230, 140, 30, 255)
    p[(8, 6)] = (230, 140, 30, 255)
    # Fangs
    p[(6, 0)] = (180, 170, 150, 255)
    p[(8, 0)] = (180, 170, 150, 255)
    # Legs — dark earthy
    for px in [0, 1]:
        for py in range(h): p[(px, py)] = (35, 28, 22, 255)
    for px in [2, 3]:
        for py in range(h): p[(px, py)] = (42, 33, 27, 255)
    for px in [13, 14]:
        for py in range(h): p[(px, py)] = (35, 28, 22, 255)
    for px in [10, 11, 12]:
        for py in range(h): p[(px, py)] = (42, 33, 27, 255)
    return w, h, p


def skin_cave_troll():
    """Cave troll: mossy grey-green rocky hide, amber eyes, moss patches.
    Grid: x=[-6,5] (w=12), y=[0,16] (h=17). min_x=-6, min_y=0.
    px=gx+6, py=gy."""
    w, h = 12, 17
    p = {}
    rock = (90, 110, 80, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = rock

    # Horns — darker stone (top py 15-16)
    for py in range(15, 17):
        for px in range(w): p[(px, py)] = (70, 85, 60, 255)
    # Head (py 12-14)
    for py in range(12, 15):
        for px in range(w): p[(px, py)] = (85, 105, 75, 255)
    # Dull amber eyes (py 14, proportional: center-left/right of 12-wide head)
    p[(4, 14)] = (180, 150, 50, 255)
    p[(7, 14)] = (180, 150, 50, 255)
    # Jaw — darker stone (py 12)
    for px in range(3, 9): p[(px, 12)] = (70, 80, 60, 255)
    # Darker cracks/veins at joints (proportional to h=17 vs old h=21)
    for py in [3, 6, 9, 11]:
        for px in range(2, 10): p[(px, py)] = (60, 75, 50, 255)
    # Barrel chest (py 7-8)
    for px in range(3, 9):
        p[(px, 7)] = (95, 115, 85, 255)
        p[(px, 8)] = (95, 115, 85, 255)
    # Moss patches scattered
    p[(2, 6)]  = (50, 100, 40, 255)
    p[(5, 10)] = (50, 100, 40, 255)
    p[(8, 4)]  = (50, 100, 40, 255)
    p[(3, 11)] = (50, 100, 40, 255)
    p[(7, 8)]  = (50, 100, 40, 255)
    # Fists (py 3-4)
    for py in [3, 4]:
        p[(0, py)] = (75, 90, 65, 255); p[(1, py)] = (75, 90, 65, 255)
        p[(10, py)] = (75, 90, 65, 255); p[(11, py)] = (75, 90, 65, 255)
    # Feet (py 0-2)
    for py in range(0, 3):
        for px in range(w): p[(px, py)] = (65, 78, 55, 255)
    return w, h, p


def skin_pit_fiend():
    """Pit Fiend: dark obsidian body, bat wings (burgundy membrane),
    magma crack veins, burning orange eyes, curved horns.
    Grid: x=[-7,6] (w=14), y=[0,23] (h=24). min_x=-7, min_y=0.
    px=gx+7, py=gy."""
    w, h = 14, 24
    p = {}
    obsidian = (30, 25, 35, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = obsidian

    # Horns (py 22-23) — dark purple-black
    for py in range(22, 24):
        for px in range(w):
            p[(px, py)] = (25, 20, 28, 255)

    # Head (py 18-21)
    for py in range(18, 22):
        for px in range(w):
            p[(px, py)] = (35, 28, 38, 255)

    # Burning orange eyes — gx -2=px 5, gx +2=px 9 at py 20
    p[(5, 20)] = (240, 140, 20, 255)
    p[(9, 20)] = (240, 140, 20, 255)

    # Wing membrane (outer columns, py 11-16) — deep burgundy
    # gx -7=px 0, gx -4=px 3, gx 3=px 10, gx 6=px 13
    for py in range(11, 17):
        for px in [0, 1, 2, 3]:
            p[(px, py)] = (80, 20, 25, 255)
        for px in [10, 11, 12, 13]:
            p[(px, py)] = (80, 20, 25, 255)

    # Wing bone struts — darker burgundy
    for py in [12, 13, 14, 15]:
        p[(0, py)] = (50, 15, 18, 255)
        p[(13, py)] = (50, 15, 18, 255)

    # Magma crack veins on torso (alternating pattern)
    # Torso gx -3..3 = px 4..10
    for py in [10, 12, 14]:
        for px in range(4, 11):
            if (px + py) % 2 == 0:
                p[(px, py)] = (220, 100, 20, 255)

    # Barrel chest — slightly lighter obsidian (gx -2..2=px 5..9, py 12-13)
    for py in [12, 13]:
        for px in range(5, 10):
            if p[(px, py)] != (220, 100, 20, 255):  # don't overwrite veins
                p[(px, py)] = (38, 32, 42, 255)

    # Dark extremities — hooves and lower legs (py 0-3)
    for py in range(0, 4):
        for px in range(w):
            p[(px, py)] = (20, 18, 25, 255)

    # Hooves — ember glow
    for px in range(w):
        p[(px, 0)] = (40, 20, 10, 255)

    # Fists — dark charcoal (gx -5..-4=px 2..3, gx 3..4=px 10..11 at py 6-7)
    for py in [6, 7]:
        for px in [2, 3]:
            p[(px, py)] = (22, 18, 26, 255)
        for px in [10, 11]:
            p[(px, py)] = (22, 18, 26, 255)

    # Tail (py 5-9) — darker obsidian (gx 0=px 7)
    for py in range(5, 10):
        p[(7, py)] = (25, 20, 30, 255)

    return w, h, p


def skin_hellforge_smith():
    """Hellforge Smith: soot-black body, glowing forge-arm (right), iron apron,
    forge bellows (rusted copper), ember eyes, massive shoulder hump.
    Grid: x=[-6,6] (w=13), y=[0,16] (h=17). min_x=-6, min_y=0.
    px=gx+6, py=gy."""
    w, h = 13, 17
    p = {}
    soot = (40, 35, 30, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = soot

    # Brow ridge / head (py 14-16) — lighter soot for face definition
    for py in range(14, 17):
        for px in range(w):
            p[(px, py)] = (45, 38, 32, 255)

    # Red ember eyes — gx -1=px 5, gx +1=px 7 at py 15
    p[(5, 15)] = (200, 60, 20, 255)
    p[(7, 15)] = (200, 60, 20, 255)

    # Shoulder hump (py 12-13) — darker soot
    for py in [12, 13]:
        for px in range(w):
            p[(px, py)] = (35, 30, 25, 255)

    # Iron-grey apron (front, gx -3..3=px 3..9, py 5-10)
    for py in range(5, 11):
        for px in range(3, 10):
            p[(px, py)] = (120, 125, 130, 255)

    # Glowing orange forge-arm on right side (gx 4..5=px 10..11, py 3-11)
    for py in range(3, 12):
        p[(10, py)] = (200, 120, 30, 255)
        p[(11, py)] = (200, 120, 30, 255)

    # Hammer head extra glow (gx 4..6=px 10..12, py 3-4) — brighter orange
    for py in range(3, 5):
        p[(10, py)] = (230, 150, 40, 255)
        p[(11, py)] = (230, 150, 40, 255)
        p[(12, py)] = (230, 150, 40, 255)

    # Forge bellows — rusted copper (gx -3..-2=px 3..4, gx 1..2=px 7..8, py 8-11)
    for py in range(8, 12):
        for px in [3, 4]:
            p[(px, py)] = (140, 90, 50, 255)
        for px in [7, 8]:
            p[(px, py)] = (140, 90, 50, 255)

    # Left arm — normal soot (gx -6..-5=px 0..1, py 3-11)
    for py in range(3, 12):
        p[(0, py)] = (35, 30, 25, 255)
        p[(1, py)] = (35, 30, 25, 255)

    # Boots — darkest charcoal (py 0-1)
    for py in range(0, 2):
        for px in range(w):
            p[(px, py)] = (30, 25, 20, 255)

    # Legs — dark (py 2-3)
    for py in range(2, 4):
        for px in range(w):
            if p[(px, py)] == soot:
                p[(px, py)] = (35, 30, 25, 255)

    return w, h, p


def skin_succubus():
    """Succubus skin — grid 7x18, matching harpy mesh (gx -3..3, gy 0..17).
    High-contrast cute harpy: pale lavender skin on face/chest/hips,
    dark purple-black on waist/lower body, magenta eyes, violet runes,
    bright horn tips, hair detail, tail glow."""
    w, h = 7, 18
    # px = gx+3, py = gy
    p = {}
    dark = (35, 15, 40, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = dark

    # --- Horn tips (gy 17) -> py 17 ---
    p[(0, 17)] = (220, 50, 160, 255)
    p[(6, 17)] = (220, 50, 160, 255)
    # Horn bases (gy 16) -> py 16
    p[(1, 16)] = (160, 35, 100, 255)
    p[(5, 16)] = (160, 35, 100, 255)

    # --- Hair (gy 15) -> py 15 — dark purple-red ---
    for px in range(2, 5): p[(px, 15)] = (80, 25, 55, 255)

    # --- Head (gy 12-14) -> py 12-14 — pale lavender skin ---
    for py in [13, 14]:
        for px in range(2, 5): p[(px, py)] = (150, 120, 145, 255)
    # Lower face/jaw
    for px in range(2, 5): p[(px, 12)] = (140, 110, 135, 255)
    # Bright magenta eyes (gy 14)
    p[(2, 14)] = (255, 40, 200, 255)
    p[(4, 14)] = (255, 40, 200, 255)
    # Mouth
    p[(3, 12)] = (120, 70, 90, 255)

    # --- Neck (gy 11) — transition ---
    p[(3, 11)] = (100, 70, 95, 255)

    # --- Shoulder nubs (gy 10) — medium, wing attach ---
    p[(0, 10)] = (90, 45, 75, 255)
    p[(6, 10)] = (90, 45, 75, 255)

    # --- Chest/bust (gy 8-10) — pale lavender skin, prominent ---
    for py in [8, 9, 10]:
        for px in range(1, 6): p[(px, py)] = (140, 108, 132, 255)
    # Bust highlights — lighter at center (gy 8-9)
    for py in [8, 9]:
        p[(2, py)] = (160, 130, 150, 255)
        p[(3, py)] = (165, 135, 155, 255)
        p[(4, py)] = (160, 130, 150, 255)
    # Bust line shadow underneath
    for px in range(1, 6): p[(px, 8)] = (130, 98, 122, 255)

    # Violet runes on torso — glow against pale skin
    p[(3, 10)] = (180, 60, 220, 255)
    p[(2, 9)]  = (160, 50, 200, 255)
    p[(4, 9)]  = (160, 50, 200, 255)
    p[(3, 8)]  = (180, 60, 220, 255)

    # --- Narrow waist (gy 7) — dark pinch, defines hourglass ---
    for px in range(2, 5): p[(px, 7)] = (55, 28, 50, 255)

    # --- Wide hips (gy 5-6) — pale skin, curvy ---
    for py in [5, 6]:
        for px in range(1, 6): p[(px, py)] = (135, 105, 128, 255)
    # Hip highlights
    p[(2, 6)] = (145, 115, 138, 255)
    p[(4, 6)] = (145, 115, 138, 255)

    # --- Tapered pelvis (gy 3-4) — transitions dark ---
    for py in [3, 4]:
        for px in range(2, 5): p[(px, py)] = (70, 35, 60, 255)

    # --- Lower taper (gy 1-2) — dark ---
    p[(2, 2)] = (50, 25, 45, 255)
    p[(3, 2)] = (50, 25, 45, 255)
    p[(4, 2)] = (50, 25, 45, 255)
    p[(3, 1)] = (40, 20, 38, 255)

    # --- Terminus (gy 0) ---
    p[(3, 0)] = (30, 12, 28, 255)

    # --- Tail (gy 2-6) — magenta glow to bright tip ---
    p[(3, 6)] = (90, 28, 70, 255)
    p[(3, 5)] = (110, 35, 85, 255)
    p[(3, 4)] = (140, 45, 105, 255)
    p[(3, 3)] = (170, 55, 125, 255)
    p[(3, 2)] = (210, 65, 155, 255)   # bright tail tip

    return w, h, p


def skin_entropy_weaver():
    """Entropy weaver: dark indigo body, purple void-glow runes, bright purple eyes.
    Grid: x=[-3,3] (w=7), y=[0,17] (h=18). min_x=-3, min_y=0.
    px=gx+3, py=gy."""
    w, h = 7, 18
    p = {}
    base = (40, 35, 70, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Skull (top py 16-17)
    for py in [16, 17]:
        for px in range(1, 6): p[(px, py)] = (50, 45, 80, 255)
    # Bright purple eyes (py 16)
    p[(2, 16)] = (170, 60, 220, 255)
    p[(4, 16)] = (170, 60, 220, 255)
    # Purple void-glow runes on torso (proportional to h=18)
    p[(2, 9)]  = (140, 50, 200, 255)
    p[(4, 10)] = (140, 50, 200, 255)
    p[(3, 8)]  = (140, 50, 200, 255)
    # Spine (py 5-12)
    for py in range(5, 13): p[(3, py)] = (30, 28, 55, 255)
    # Ribs (py 6,8,10 — spread across taller torso)
    for py in [6, 8, 10]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (35, 32, 62, 255)
    # Rib gaps (py 9)
    for px in [1, 2, 4, 5]: p[(px, 9)] = (15, 12, 30, 255)
    # Arms (py 3-11)
    for py in range(3, 12):
        p[(0, py)] = (35, 30, 60, 255)
        p[(6, py)] = (35, 30, 60, 255)
    # Pale grey hands at extremities (py 0-2)
    for py in range(0, 3):
        p[(0, py)] = (150, 145, 160, 255)
        p[(6, py)] = (150, 145, 160, 255)
    # Pelvis (py 4)
    for px in range(2, 5): p[(px, 4)] = (35, 30, 60, 255)
    # Legs (py 1-3, dark indigo)
    for px in [1, 4]:
        p[(px, 2)] = base; p[(px, 3)] = base
        p[(px, 0)] = (30, 25, 55, 255); p[(px, 1)] = (32, 28, 58, 255)
    return w, h, p


def skin_nullifier():
    """Bat-rig nullifier: jet black body, white-blue energy veins, white void eyes."""
    w, h = 5, 10
    p = {}
    black = (20, 20, 25, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = black

    # Faint dark blue tint on wings
    for py in range(h):
        p[(0, py)] = (25, 25, 40, 255)
        p[(4, py)] = (25, 25, 40, 255)
    # Lighter belly
    for py in range(0, 4):
        for px in range(1, 4): p[(px, py)] = (28, 28, 35, 255)
    # White-blue energy veins
    p[(2, 5)] = (180, 200, 240, 255)
    p[(1, 3)] = (180, 200, 240, 255)
    p[(3, 7)] = (180, 200, 240, 255)
    # White void eyes — same position as base bat (py=6)
    p[(1, 6)] = (220, 230, 250, 255)
    p[(3, 6)] = (220, 230, 250, 255)
    # Ears
    p[(1, 8)] = (30, 30, 42, 255)
    p[(3, 8)] = (30, 30, 42, 255)
    # Snout
    p[(2, 4)] = (25, 25, 32, 255)
    p[(2, 5)] = (180, 200, 240, 255)  # energy vein overlaps snout
    # Claws
    for px in [1, 3]:
        p[(px, 0)] = (15, 15, 20, 255)
        p[(px, 1)] = (15, 15, 20, 255)
    # Wing shoulders
    p[(0, 2)] = (28, 28, 45, 255)
    p[(4, 2)] = (28, 28, 45, 255)
    return w, h, p


def skin_mind_flayer():
    """Wraith-rig mind flayer (5x18 grid): deep void-purple body, sickly green
    drain-glow eyes, dark tentacle streaks on lower body, magenta energy veins."""
    w, h = 5, 18
    p = {}
    # Deep void purple, semi-transparent
    void = (45, 20, 60, 160)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = void

    # Tentacle streaks on lower body (gy 0-7) — darker, dripping
    for py in range(0, 8):
        p[(1, py)] = (30, 10, 40, 140)
        p[(3, py)] = (25, 8, 35, 130)
    # Feet — almost invisible tendrils
    for py in range(0, 2):
        for px in range(w):
            p[(px, py)] = (20, 8, 30, 100)

    # Torso magenta energy veins (gy 8-13)
    for py in [8, 10, 12]:
        for px in range(1, 4):
            if (px + py) % 3 == 0:
                p[(px, py)] = (180, 40, 140, 200)
    # Brighter core glow
    for py in range(9, 13):
        p[(2, py)] = (160, 30, 120, 190)

    # Head (gy 15-17) — slightly lighter void
    for py in range(15, 18):
        for px in range(1, 4):
            p[(px, py)] = (55, 28, 70, 180)
    # Sickly green drain-glow eyes
    p[(1, 16)] = (80, 220, 60, 230)
    p[(3, 16)] = (80, 220, 60, 230)
    # Mouth — void black
    p[(2, 15)] = (10, 5, 15, 200)
    p[(1, 15)] = (15, 8, 20, 180)
    p[(3, 15)] = (15, 8, 20, 180)

    # Arms — fading tendrils
    for py in range(4, 12):
        p[(0, py)] = (35, 15, 50, 130)
        p[(4, py)] = (35, 15, 50, 130)

    return w, h, p


def skin_phase_ripper():
    """Bat-rig phase ripper: deep purple-black body, bright magenta phase-energy cracks,
    glowing magenta eyes, wing edges shimmer with alternating phase energy."""
    w, h = 5, 10
    p = {}
    # Deep purple-black base
    base = (25, 10, 35, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Wing edges — phase shimmer (alternating bright/dark purple)
    for py in range(h):
        bright = (py % 2 == 0)
        p[(0, py)] = (140, 40, 180, 255) if bright else (50, 15, 65, 255)
        p[(4, py)] = (140, 40, 180, 255) if bright else (50, 15, 65, 255)

    # Subtle phase-energy veins across body (dim, won't read as eyes)
    p[(2, 3)] = (70, 25, 80, 255)
    p[(1, 5)] = (65, 20, 75, 255)
    p[(3, 7)] = (70, 25, 80, 255)
    p[(2, 8)] = (65, 20, 75, 255)

    # Belly — slightly lighter
    for py in range(1, 4):
        for px in range(1, 4):
            p[(px, py)] = (35, 15, 45, 255)

    # Glowing magenta eyes
    p[(1, 6)] = (255, 80, 220, 255)
    p[(3, 6)] = (255, 80, 220, 255)
    # Ears
    p[(1, 8)] = (40, 15, 55, 255)
    p[(3, 8)] = (40, 15, 55, 255)
    # Snout
    p[(2, 4)] = (30, 12, 40, 255)
    # Claws — dark void
    for px in [1, 3]:
        p[(px, 0)] = (15, 5, 20, 255)
        p[(px, 1)] = (15, 5, 20, 255)

    return w, h, p


def skin_abyssal_titan():
    """Abyssal titan: deep void-black body, faint purple runes, ice-blue eyes.
    Grid: x=[-7,6] (w=14), y=[0,22] (h=23). min_x=-7, min_y=0.
    px=gx+7, py=gy."""
    w, h = 14, 23
    p = {}
    void = (25, 22, 35, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = void

    # Horns (top py 21-22)
    for py in range(21, 23):
        for px in range(w): p[(px, py)] = (20, 18, 28, 255)
    # Head (py 17-20)
    for py in range(17, 21):
        for px in range(w): p[(px, py)] = (28, 25, 38, 255)
    # Ice-blue eyes (py 19, proportional: gx -2=px 5, gx +2=px 9)
    p[(5, 19)] = (120, 180, 240, 255)
    p[(9, 19)] = (120, 180, 240, 255)
    # Frost-white jaw (py 17-18, center px 4-9)
    for px in range(4, 10):
        p[(px, 17)] = (180, 190, 210, 255)
        p[(px, 18)] = (180, 190, 210, 255)
    # Faint purple rune-glow on chest (py 11-13, center)
    p[(5, 11)] = (80, 50, 140, 255)
    p[(7, 12)] = (80, 50, 140, 255)
    p[(6, 13)] = (80, 50, 140, 255)
    # Barrel chest (py 11-12, center px 4-9)
    for px in range(4, 10):
        p[(px, 11)] = (28, 25, 38, 255)
        p[(px, 12)] = (28, 25, 38, 255)
    # Fists (py 4-5, outer columns)
    for py in [4, 5]:
        p[(0, py)] = (20, 18, 28, 255); p[(1, py)] = (20, 18, 28, 255)
        p[(12, py)] = (20, 18, 28, 255); p[(13, py)] = (20, 18, 28, 255)
    # Hooves (py 0-2)
    for py in range(0, 3):
        for px in range(w): p[(px, py)] = (18, 16, 25, 255)
    return w, h, p


# ---------------------------------------------------------------------------
# Equipment skin textures (4x4 grid) — full UV (0,0)-(1,1) per face.
# Row 0 = bottom, row 3 = top (Y-flipped on write like all other skins).
# ---------------------------------------------------------------------------

def skin_weapon_melee_tex():
    """Steel sword: bright steel blade top half, dark grip bottom, edge highlight right column."""
    w, h = 4, 4
    p = {}
    steel      = (180, 180, 195, 255)
    grip       = (60,  45,  30,  255)
    highlight  = (220, 220, 230, 255)
    for py in range(h):
        for px in range(w):
            # Top two rows = blade, bottom two = grip
            p[(px, py)] = steel if py >= 2 else grip
    # Edge highlight on right column (all rows)
    for py in range(h):
        p[(3, py)] = highlight
    return w, h, p


def skin_weapon_hitscan_tex():
    """Gunmetal pistol: dark metal barrel top half, wood grip bottom, barrel highlight."""
    w, h = 4, 4
    p = {}
    metal     = (50, 50, 55, 255)
    wood      = (100, 70, 40, 255)
    barrel_hi = (80, 80, 90, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = metal if py >= 2 else wood
    # Barrel highlight strip on column 1
    for py in range(2, 4):
        p[(1, py)] = barrel_hi
    return w, h, p


def skin_weapon_projectile_tex():
    """Wood bow: warm brown with darker grain and string line on right edge."""
    w, h = 4, 4
    p = {}
    brown  = (140, 100, 55, 255)
    grain  = (110,  75, 40, 255)
    string = (200, 190, 170, 255)
    for py in range(h):
        for px in range(w):
            # Alternating grain lines on columns 0 and 2
            p[(px, py)] = grain if px in (0, 2) else brown
    # String line on right edge
    for py in range(h):
        p[(3, py)] = string
    return w, h, p


def skin_weapon_staff_tex():
    """Magic staff: dark wood shaft, glowing purple crystal top row."""
    w, h = 4, 4
    p = {}
    dark_wood = (80, 55, 30, 255)
    crystal   = (140, 60, 200, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = dark_wood
    # Glowing crystal on top row (py=3)
    for px in range(w):
        p[(px, 3)] = crystal
    return w, h, p


def skin_weapon_molotov_tex():
    """Molotov cocktail: glass body with orange liquid in bottom half."""
    w, h = 4, 4
    p = {}
    glass  = (140, 160, 150, 255)
    liquid = (220, 120,  30, 255)
    for py in range(h):
        for px in range(w):
            # Bottom two rows = liquid, top two = glass
            p[(px, py)] = liquid if py < 2 else glass
    return w, h, p


def skin_weapon_sword_tex():
    """Sword: bright steel blade top half, brown leather grip bottom, edge highlight."""
    w, h = 4, 4
    p = {}
    steel     = (180, 180, 195, 255)
    grip      = (90, 60, 35, 255)
    highlight = (220, 220, 230, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = steel if py >= 2 else grip
    # Edge highlight on right column
    for py in range(h):
        p[(3, py)] = highlight
    return w, h, p


def skin_weapon_dagger_tex():
    """Dagger: darker steel blade, black grip, thin highlight on column 2."""
    w, h = 4, 4
    p = {}
    steel     = (140, 140, 160, 255)
    grip      = (30, 25, 20, 255)
    highlight = (180, 180, 195, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = steel if py >= 2 else grip
    # Thin highlight on column 2 (not right edge — narrower blade feel)
    for py in range(2, 4):
        p[(2, py)] = highlight
    return w, h, p


def skin_weapon_axe_tex():
    """Axe: grey iron blade top, reddish-brown haft bottom, wide blade highlight."""
    w, h = 4, 4
    p = {}
    iron = (150, 150, 155, 255)
    haft = (120, 65, 30, 255)
    edge = (200, 200, 210, 255)
    for py in range(h):
        for px in range(w):
            # Top two rows = blade, bottom two = haft
            p[(px, py)] = iron if py >= 2 else haft
    # Wide blade highlight — entire top row
    for px in range(w):
        p[(px, 3)] = edge
    return w, h, p


def skin_weapon_cleaver_tex():
    """Cleaver: dark steel blade, blood-red edge highlight, dark wood handle."""
    w, h = 4, 4
    p = {}
    dark_steel = (90, 90, 100, 255)
    blood_edge = (160, 30, 30, 255)
    handle     = (60, 40, 25, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = dark_steel if py >= 2 else handle
    # Blood-red cutting edge — top row
    for px in range(w):
        p[(px, 3)] = blood_edge
    return w, h, p


def skin_weapon_claymore_tex():
    """Claymore: blue-steel blade, leather wrap grip, cross-guard accent row."""
    w, h = 4, 4
    p = {}
    blue_steel = (170, 175, 200, 255)
    leather    = (100, 75, 45, 255)
    accent     = (130, 120, 90, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = blue_steel if py >= 2 else leather
    # Cross-guard accent on row 2 (boundary between grip and blade)
    for px in range(w):
        p[(px, 2)] = accent
    return w, h, p


def skin_weapon_pistol_tex():
    """Pistol: gunmetal body, wood grip bottom, barrel highlight strip."""
    w, h = 4, 4
    p = {}
    metal     = (50, 50, 55, 255)
    wood      = (100, 70, 40, 255)
    barrel_hi = (80, 80, 90, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = metal if py >= 2 else wood
    # Barrel highlight on column 1
    for py in range(2, 4):
        p[(1, py)] = barrel_hi
    return w, h, p


def skin_weapon_smg_tex():
    """SMG: dark polymer body, grey metal accents, magazine block pattern."""
    w, h = 4, 4
    p = {}
    polymer = (35, 35, 40, 255)
    metal   = (70, 70, 75, 255)
    mag     = (45, 45, 50, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = polymer
    # Metal accents on top row
    for px in range(w):
        p[(px, 3)] = metal
    # Magazine block pattern on bottom-left
    p[(0, 0)] = mag
    p[(1, 0)] = mag
    p[(0, 1)] = mag
    p[(1, 1)] = mag
    return w, h, p


def skin_weapon_carbine_tex():
    """Carbine: matte black body, brown wood furniture, green scope dot."""
    w, h = 4, 4
    p = {}
    black = (25, 25, 30, 255)
    wood  = (90, 65, 35, 255)
    scope = (40, 120, 40, 255)
    for py in range(h):
        for px in range(w):
            # Top two rows = barrel (black), bottom two = furniture (wood)
            p[(px, py)] = black if py >= 2 else wood
    # Scope dot — green pixel at top-right
    p[(3, 3)] = scope
    return w, h, p


def skin_weapon_revolver_tex():
    """Revolver: silver body, ivory grip, cylinder drum pattern."""
    w, h = 4, 4
    p = {}
    silver   = (160, 160, 170, 255)
    ivory    = (200, 190, 170, 255)
    cylinder = (120, 120, 130, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = silver if py >= 2 else ivory
    # Cylinder drum pattern — darker block in middle
    p[(1, 2)] = cylinder
    p[(2, 2)] = cylinder
    p[(1, 3)] = cylinder
    p[(2, 3)] = cylinder
    return w, h, p


def skin_weapon_bow_tex():
    """Bow: warm brown wood, darker grain lines, cream string on right edge."""
    w, h = 4, 4
    p = {}
    brown  = (140, 100, 55, 255)
    grain  = (110, 75, 40, 255)
    string = (200, 190, 170, 255)
    for py in range(h):
        for px in range(w):
            # Alternating grain on columns 0 and 2
            p[(px, py)] = grain if px in (0, 2) else brown
    # String on right edge
    for py in range(h):
        p[(3, py)] = string
    return w, h, p


def skin_weapon_crossbow_tex():
    """Crossbow: dark wood body, iron fittings, bolt channel highlight."""
    w, h = 4, 4
    p = {}
    dark_wood = (80, 55, 30, 255)
    iron      = (100, 100, 110, 255)
    bolt      = (130, 130, 140, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = dark_wood
    # Iron fittings on top row
    for px in range(w):
        p[(px, 3)] = iron
    # Bolt channel — highlight strip on column 1
    for py in range(h):
        p[(1, py)] = bolt
    return w, h, p


def skin_weapon_throwing_knife_tex():
    """Throwing knife: bright steel all-blade pattern, small dark pommel dot."""
    w, h = 4, 4
    p = {}
    steel  = (190, 190, 205, 255)
    edge   = (220, 220, 235, 255)
    pommel = (50, 40, 30, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = steel
    # Edge highlights on columns 0 and 3
    for py in range(h):
        p[(0, py)] = edge
        p[(3, py)] = edge
    # Pommel dot — bottom-center
    p[(1, 0)] = pommel
    p[(2, 0)] = pommel
    return w, h, p


def skin_shock_bolt_tex():
    """Shock bolt projectile: bright white-blue electric bolt."""
    w, h = 4, 4
    p = {}
    core = (180, 200, 255, 255)   # bright white-blue
    glow = (80, 140, 255, 255)    # electric blue
    tip  = (220, 230, 255, 255)   # near-white tip
    for py in range(h):
        for px in range(w):
            p[(px, py)] = glow
    # Bright core down the center columns
    for py in range(h):
        p[(1, py)] = core
        p[(2, py)] = core
    # Glowing tip at top
    for px in range(w):
        p[(px, 3)] = tip
    return w, h, p


def skin_turret_tex():
    """Sentry turret: metallic grey body, darker base, red targeting lens."""
    w, h = 4, 4
    p = {}
    metal  = (140, 140, 150, 255)
    dark   = (80, 80, 90, 255)
    barrel = (100, 100, 110, 255)
    lens   = (220, 40, 30, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = dark if py < 1 else metal
    # Barrel strip on top row
    for px in range(w):
        p[(px, 3)] = barrel
    # Red targeting lens
    p[(1, 2)] = lens
    p[(2, 2)] = lens
    return w, h, p


def skin_weapon_wand_tex():
    """Wand: dark wood shaft, glowing blue-green gem tip on top row."""
    w, h = 4, 4
    p = {}
    dark_wood = (80, 55, 30, 255)
    gem       = (60, 180, 160, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = dark_wood
    # Glowing gem on top row
    for px in range(w):
        p[(px, 3)] = gem
    return w, h, p


def skin_helmet_plate_tex():
    """Plate helmet: polished steel body, dark visor slit on row 2, rivets on corners."""
    w, h = 4, 4
    p = {}
    steel  = (165, 165, 175, 255)
    visor  = (40,  40,  50,  255)
    rivet  = (120, 120, 130, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = steel
    # Visor slit on row 2 (py=2, second from top)
    for px in range(w):
        p[(px, 2)] = visor
    # Rivets on corner pixels of row 3 and row 0
    p[(0, 3)] = rivet; p[(3, 3)] = rivet
    p[(0, 0)] = rivet; p[(3, 0)] = rivet
    return w, h, p


def skin_helmet_leather_tex():
    """Leather helmet: brown body with stitching dots and lighter brow band."""
    w, h = 4, 4
    p = {}
    leather  = (130, 90,  50,  255)
    stitch   = (90,  60,  35,  255)
    band     = (150, 110, 65,  255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = leather
    # Stitching dots on row 1 at columns 1 and 2
    p[(1, 1)] = stitch; p[(2, 1)] = stitch
    # Lighter brow band across top row
    for px in range(w):
        p[(px, 3)] = band
    return w, h, p


def skin_armor_plate_tex():
    """Steel breastplate: steel body, dark center buckle column, edge trim."""
    w, h = 4, 4
    p = {}
    steel  = (160, 160, 170, 255)
    buckle = (80,  80,  90,  255)
    trim   = (140, 140, 150, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = steel
    # Center buckle on column 1 and 2, middle rows
    for py in range(1, 3):
        p[(1, py)] = buckle; p[(2, py)] = buckle
    # Edge trim on left and right columns
    for py in range(h):
        p[(0, py)] = trim; p[(3, py)] = trim
    return w, h, p


def skin_armor_leather_tex():
    """Leather armor: brown body with cross-stitch pattern and lighter trim."""
    w, h = 4, 4
    p = {}
    brown  = (120, 85, 45,  255)
    stitch = (90,  60, 30,  255)
    trim   = (145, 105, 60, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = brown
    # Cross-stitch: diagonal pattern
    p[(0, 3)] = stitch; p[(1, 2)] = stitch
    p[(2, 1)] = stitch; p[(3, 0)] = stitch
    # Lighter trim on top row
    for px in range(w):
        p[(px, 3)] = trim
    return w, h, p


def skin_armor_cloth_tex():
    """Purple cloth armor: purple body with gold embroidery dots."""
    w, h = 4, 4
    p = {}
    cloth = (100, 80,  130, 255)
    gold  = (180, 150, 60,  255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = cloth
    # Gold embroidery dots in a 2x2 checkerboard pattern
    p[(1, 1)] = gold; p[(2, 2)] = gold
    p[(0, 2)] = gold; p[(3, 1)] = gold
    return w, h, p


def skin_boots_plate_tex():
    """Plate boots: steel body, dark sole on bottom row, knee highlight on top row."""
    w, h = 4, 4
    p = {}
    steel = (155, 155, 165, 255)
    sole  = (50,  45,  40,  255)
    knee  = (175, 175, 185, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = steel
    # Dark sole on bottom row
    for px in range(w):
        p[(px, 0)] = sole
    # Knee guard highlight on top row
    for px in range(w):
        p[(px, 3)] = knee
    return w, h, p


def skin_boots_leather_tex():
    """Leather boots: brown body with lace dots and dark sole."""
    w, h = 4, 4
    p = {}
    brown = (115, 80, 40,  255)
    lace  = (80,  55, 28,  255)
    sole  = (45,  35, 20,  255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = brown
    # Lace dots on rows 1 and 2 at columns 1 and 2
    p[(1, 2)] = lace; p[(2, 2)] = lace
    p[(1, 1)] = lace; p[(2, 1)] = lace
    # Dark sole on bottom row
    for px in range(w):
        p[(px, 0)] = sole
    return w, h, p


def skin_ring_bone_tex():
    """Bone ring: white bone band with carved line and dark center."""
    w, h = 4, 4
    p = {}
    bone   = (210, 200, 185, 255)
    carved = (170, 160, 140, 255)
    center = (140, 130, 115, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bone
    # Carved line across middle rows (horizontal band)
    for px in range(w):
        p[(px, 1)] = carved; p[(px, 2)] = carved
    # Dark center on 2x2 middle
    p[(1, 1)] = center; p[(2, 1)] = center
    p[(1, 2)] = center; p[(2, 2)] = center
    return w, h, p


def skin_ring_gold_tex():
    """Gold ring: golden band with bright gemstone center pixel and shadow."""
    w, h = 4, 4
    p = {}
    gold   = (210, 180, 60,  255)
    gem    = (100, 200, 255, 255)
    shadow = (170, 145, 45,  255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = gold
    # Darker shadow on bottom row
    for px in range(w):
        p[(px, 0)] = shadow
    # Bright gemstone single center pixel
    p[(1, 2)] = gem  # slightly off-center for asymmetry interest
    return w, h, p


def skin_shield_wood_tex():
    """Wood shield: plank texture with grain, metal boss center."""
    w, h = 4, 4
    p = {}
    plank = (130, 95,  50,  255)
    grain = (100, 70,  35,  255)
    boss  = (160, 160, 170, 255)
    for py in range(h):
        for px in range(w):
            # Vertical plank grain lines on columns 0 and 3
            p[(px, py)] = grain if px in (0, 3) else plank
    # Metal boss on 2x2 center
    p[(1, 1)] = boss; p[(2, 1)] = boss
    p[(1, 2)] = boss; p[(2, 2)] = boss
    return w, h, p


def skin_shield_plate_tex():
    """Steel shield: steel body with red emblem center and golden border."""
    w, h = 4, 4
    p = {}
    steel  = (155, 155, 165, 255)
    emblem = (120, 50,  40,  255)  # red
    border = (190, 165, 55,  255)  # gold
    for py in range(h):
        for px in range(w):
            p[(px, py)] = steel
    # Golden border on outermost ring
    for px in range(w):
        p[(px, 0)] = border; p[(px, 3)] = border
    for py in range(1, 3):
        p[(0, py)] = border; p[(3, py)] = border
    # Red emblem on 2x2 center
    p[(1, 1)] = emblem; p[(2, 1)] = emblem
    p[(1, 2)] = emblem; p[(2, 2)] = emblem
    return w, h, p


# ---------------------------------------------------------------------------
# Minion portrait icons (8x8) — top-down HUD portraits for summoned minions.
# px=0 is left, py=0 is bottom (Y-flipped on write like all other skins).
# ---------------------------------------------------------------------------

def skin_icon_drone():
    """8x8 portrait matching the in-game combat drone — dark metallic spider with red eyes."""
    w, h = 8, 8
    bg    = (15, 15, 20, 255)
    body  = (90, 85, 100, 255)      # dark metallic (prop_iron on spider mesh)
    body2 = (70, 65, 80, 255)       # darker abdomen
    leg   = (60, 55, 70, 255)       # dark metal legs
    eye   = (220, 40, 30, 255)      # bright red (matches spider_skin eye stalks)
    p = {(px, py): bg for py in range(h) for px in range(w)}

    # Fat spider body — 4x3 center
    for py in range(2, 5):
        for px in range(2, 6):
            p[(px, py)] = body
    for px in range(3, 5):
        p[(px, 2)] = body2; p[(px, 3)] = body2

    # 8 legs radiating from body
    p[(1, 5)] = leg; p[(0, 6)] = leg   # front-left
    p[(6, 5)] = leg; p[(7, 6)] = leg   # front-right
    p[(1, 1)] = leg; p[(0, 0)] = leg   # back-left
    p[(6, 1)] = leg; p[(7, 0)] = leg   # back-right
    p[(1, 3)] = leg; p[(0, 3)] = leg   # mid-left
    p[(6, 3)] = leg; p[(7, 3)] = leg   # mid-right

    # Two bright red eyes at front
    p[(3, 5)] = eye; p[(4, 5)] = eye

    return w, h, p


def skin_icon_swarm():
    """8x8 portrait matching the in-game swarm drone — tiny dark metallic bat body."""
    w, h = 8, 8
    bg   = (15, 15, 20, 255)
    body = (80, 75, 90, 255)        # dark metallic (prop_iron on bat mesh)
    ear  = (60, 55, 70, 255)        # darker ear/top detail
    belly = (95, 90, 105, 255)      # slightly lighter belly
    eye  = (150, 180, 230, 255)     # pale blue (matches void_bat eye style)
    p = {(px, py): bg for py in range(h) for px in range(w)}

    # Small bat body — 4x3 center
    for py in range(2, 5):
        for px in range(2, 6):
            p[(px, py)] = body
    # Lighter belly
    p[(3, 2)] = belly; p[(4, 2)] = belly
    # Ear points
    p[(2, 5)] = ear; p[(5, 5)] = ear
    p[(1, 6)] = ear; p[(6, 6)] = ear

    # Blue eyes
    p[(3, 4)] = eye; p[(4, 4)] = eye

    return w, h, p


def skin_icon_turret():
    """8x8 portrait matching the in-game turret — dark metallic spider base with barrel."""
    w, h = 8, 8
    bg     = (15, 15, 20, 255)
    base   = (85, 80, 95, 255)      # dark metallic base (same as drone)
    base2  = (70, 65, 80, 255)      # darker center
    barrel = (100, 100, 115, 255)   # lighter barrel
    dot    = (220, 50, 30, 255)     # red targeting dot
    p = {(px, py): bg for py in range(h) for px in range(w)}

    # Spider-like base — 4x4 with cut corners
    for py in range(2, 6):
        for px in range(2, 6):
            p[(px, py)] = base
    p[(2, 2)] = bg; p[(5, 2)] = bg
    p[(2, 5)] = bg; p[(5, 5)] = bg
    # Darker center
    p[(3, 3)] = base2; p[(4, 3)] = base2
    p[(3, 4)] = base2; p[(4, 4)] = base2

    # Barrel extending upward
    p[(3, 5)] = barrel
    p[(3, 6)] = barrel
    p[(3, 7)] = barrel

    # Red targeting dot at barrel tip (py 7)
    p[(3, 7)] = dot

    return w, h, p


def skin_bat_wing():
    """4x4 bat wing membrane texture — dark membrane with bone structure visible."""
    w, h = 4, 4
    membrane = (40, 30, 50, 255)     # dark purple-brown membrane
    bone     = (80, 65, 75, 255)     # lighter bone/finger lines
    edge     = (25, 20, 35, 255)     # dark wing tip
    joint    = (100, 70, 55, 255)    # warm joint where wing meets body
    p = {(px, py): membrane for py in range(h) for px in range(w)}

    # Bone fingers radiating from top-left (joint) to bottom-right (tip)
    p[(0, 3)] = joint                # shoulder joint
    p[(1, 3)] = bone                 # upper bone
    p[(0, 2)] = bone                 # bone line
    p[(1, 2)] = bone                 # bone line
    p[(2, 1)] = bone                 # finger extending
    p[(3, 0)] = edge                 # wing tip
    p[(3, 1)] = edge                 # edge
    p[(0, 0)] = edge                 # lower edge
    # Thin membrane between bones (slightly lighter)
    p[(2, 2)] = (50, 38, 58, 255)    # mid-membrane
    p[(1, 1)] = (45, 34, 54, 255)    # membrane between bones
    p[(2, 3)] = (55, 42, 62, 255)    # near-body membrane

    return w, h, p


# --- Legendary glow variants ---

def skin_legendary_weapon_tex():
    """Legendary weapon: bright gold blade, white-hot edge, rune marks."""
    w, h = 4, 4
    p = {}
    gold    = (240, 210, 80,  255)
    hot     = (255, 250, 200, 255)  # white-hot edge
    rune    = (200, 170, 50,  255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = gold
    # White-hot edge on right column
    for py in range(h):
        p[(3, py)] = hot
    # Rune marks: scattered darker gold pixels
    p[(0, 1)] = rune; p[(1, 3)] = rune; p[(2, 0)] = rune
    return w, h, p


def skin_legendary_armor_tex():
    """Legendary armor: golden plate, glowing sigil center, dark trim."""
    w, h = 4, 4
    p = {}
    gold  = (230, 200, 70,  255)
    sigil = (255, 240, 150, 255)
    trim  = (180, 150, 45,  255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = gold
    # Dark trim on outer edges
    for px in range(w):
        p[(px, 0)] = trim; p[(px, 3)] = trim
    p[(0, 1)] = trim; p[(0, 2)] = trim
    p[(3, 1)] = trim; p[(3, 2)] = trim
    # Glowing sigil on 2x2 center
    p[(1, 1)] = sigil; p[(2, 1)] = sigil
    p[(1, 2)] = sigil; p[(2, 2)] = sigil
    return w, h, p


def skin_legendary_helm_tex():
    """Legendary helm: golden crown, bright jewel center, white accent."""
    w, h = 4, 4
    p = {}
    gold   = (235, 205, 75,  255)
    jewel  = (255, 100, 100, 255)
    accent = (250, 245, 220, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = gold
    # White accent on top row
    for px in range(w):
        p[(px, 3)] = accent
    # Bright jewel — single center pixel
    p[(1, 2)] = jewel
    return w, h, p


def skin_legendary_boots_tex():
    """Legendary boots: golden body, energy trail on bottom row, trim."""
    w, h = 4, 4
    p = {}
    gold   = (225, 195, 65,  255)
    energy = (200, 255, 200, 255)
    trim   = (190, 160, 50,  255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = gold
    # Energy trail on bottom row
    for px in range(w):
        p[(px, 0)] = energy
    # Trim on top row
    for px in range(w):
        p[(px, 3)] = trim
    return w, h, p


def skin_legendary_ring_tex():
    """Legendary ring: bright gold band, large 2x2 gemstone center, dark shadow."""
    w, h = 4, 4
    p = {}
    gold   = (240, 210, 80,  255)
    gem    = (150, 230, 255, 255)  # large bright gemstone
    shadow = (180, 150, 45,  255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = gold
    # Dark shadow bottom row
    for px in range(w):
        p[(px, 0)] = shadow
    # 2x2 gemstone center
    p[(1, 1)] = gem; p[(2, 1)] = gem
    p[(1, 2)] = gem; p[(2, 2)] = gem
    return w, h, p


def skin_legendary_shield_tex():
    """Legendary shield: golden body, bright emblem center, golden border."""
    w, h = 4, 4
    p = {}
    gold   = (235, 205, 75,  255)
    emblem = (255, 255, 200, 255)
    border = (210, 180, 55,  255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = gold
    # Border on outermost ring
    for px in range(w):
        p[(px, 0)] = border; p[(px, 3)] = border
    for py in range(1, 3):
        p[(0, py)] = border; p[(3, py)] = border
    # Bright emblem on 2x2 center
    p[(1, 1)] = emblem; p[(2, 1)] = emblem
    p[(1, 2)] = emblem; p[(2, 2)] = emblem
    return w, h, p


# --- Status effect icons (4x4) ---

def skin_status_poison():
    """Poison icon: green droplet on dark background."""
    w, h = 4, 4
    p = {}
    bg    = (20, 30, 20, 255)
    green = (60, 200, 60, 255)
    lime  = (120, 255, 80, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bg
    p[(1, 3)] = green; p[(2, 3)] = green
    p[(1, 2)] = lime;  p[(2, 2)] = lime
    p[(0, 1)] = green; p[(1, 1)] = green; p[(2, 1)] = green; p[(3, 1)] = green
    p[(1, 0)] = green; p[(2, 0)] = green
    return w, h, p

def skin_status_burn():
    """Burn icon: orange flame on dark background."""
    w, h = 4, 4
    p = {}
    bg     = (30, 15, 10, 255)
    orange = (255, 140, 30, 255)
    yellow = (255, 220, 60, 255)
    red    = (220, 60, 20, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bg
    p[(1, 0)] = red;    p[(2, 0)] = red
    p[(0, 1)] = orange; p[(1, 1)] = orange; p[(2, 1)] = orange; p[(3, 1)] = orange
    p[(1, 2)] = yellow; p[(2, 2)] = yellow
    p[(1, 3)] = yellow
    return w, h, p

def skin_status_freeze():
    """Freeze icon: blue snowflake/crystal on dark background."""
    w, h = 4, 4
    p = {}
    bg    = (10, 15, 35, 255)
    ice   = (100, 180, 255, 255)
    white = (220, 240, 255, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bg
    p[(1, 3)] = ice;   p[(2, 3)] = ice
    p[(0, 2)] = ice;   p[(1, 2)] = white; p[(2, 2)] = white; p[(3, 2)] = ice
    p[(0, 1)] = ice;   p[(1, 1)] = white; p[(2, 1)] = white; p[(3, 1)] = ice
    p[(1, 0)] = ice;   p[(2, 0)] = ice
    return w, h, p

def skin_status_slow():
    """Slow icon: purple snail/spiral on dark background."""
    w, h = 4, 4
    p = {}
    bg     = (20, 15, 30, 255)
    purple = (160, 80, 220, 255)
    pink   = (200, 140, 255, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bg
    p[(0, 0)] = purple; p[(1, 0)] = purple; p[(2, 0)] = purple; p[(3, 0)] = purple
    p[(0, 1)] = purple; p[(1, 1)] = pink;   p[(2, 1)] = pink
    p[(0, 2)] = purple; p[(1, 2)] = pink;   p[(2, 2)] = purple; p[(3, 2)] = purple
    p[(2, 3)] = purple; p[(3, 3)] = purple
    return w, h, p

def skin_status_invuln():
    """Invulnerable icon: golden shield on dark background."""
    w, h = 4, 4
    p = {}
    bg   = (25, 25, 15, 255)
    gold = (240, 210, 80, 255)
    bright = (255, 245, 160, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bg
    p[(1, 3)] = gold;   p[(2, 3)] = gold
    p[(0, 2)] = gold;   p[(1, 2)] = bright; p[(2, 2)] = bright; p[(3, 2)] = gold
    p[(0, 1)] = gold;   p[(1, 1)] = bright; p[(2, 1)] = bright; p[(3, 1)] = gold
    p[(1, 0)] = gold;   p[(2, 0)] = gold
    return w, h, p

# --- Boss skins (floor 10+) ---

def skin_boss_andariel():
    """Andariel — spider-woman demon. Dark chitin upper body, pale toxic lower.
    Multiple red eyes, dark carapace shoulders. Andariel mesh 11x18."""
    w, h = 11, 18
    p = {}
    chitin   = (45, 35, 25, 255)     # dark brown-black chitin (spider shell)
    darkSkin = (70, 55, 40, 255)     # dark skin between plates
    paleSkin = (140, 120, 90, 255)   # pale underbelly/face
    toxic    = (80, 140, 50, 255)    # toxic green belly/veins
    eye      = (220, 40, 30, 255)    # glowing red spider eyes
    carapace = (35, 25, 18, 255)     # near-black armor plates
    fang     = (200, 190, 170, 255)  # pale fangs/teeth
    for py in range(h):
        for px in range(w):
            p[(px, py)] = darkSkin
    # Chitin carapace on upper torso (gy=10-12) and shoulder spikes
    for py in range(10, 13):
        for px in range(w):
            p[(px, py)] = chitin
    # Shoulder spike tips (gx=0,1 and 9,10)
    for py in range(11, 13):
        p[(0, py)] = carapace; p[(1, py)] = carapace
        p[(9, py)] = carapace; p[(10, py)] = carapace
    # Narrow waist (gy=8-9) — darker
    for py in range(8, 10):
        for px in range(3, 8):
            p[(px, py)] = chitin
    # Bulbous abdomen with toxic green (gy=4-7)
    for py in range(4, 8):
        for px in range(2, 9):
            p[(px, py)] = toxic
    p[(4, 6)] = darkSkin; p[(6, 6)] = darkSkin  # vein breaks
    # Dark chitinous legs (gy=0-3)
    for py in range(0, 4):
        for px in range(w):
            p[(px, py)] = chitin
    # Pale angular face (gy=14-17)
    for px in range(3, 8):
        for py in range(15, 18):
            p[(px, py)] = paleSkin
    # 4 spider eyes
    p[(4, 16)] = eye; p[(6, 16)] = eye   # upper main eyes
    p[(4, 15)] = eye; p[(6, 15)] = eye   # lower small eyes
    # Dark crown/carapace top
    for px in range(2, 9):
        p[(px, 17)] = carapace
    # Fangs at jaw (gy=14)
    p[(4, 14)] = fang; p[(6, 14)] = fang
    # Long clawed arms — dark chitin (gx=0-1 and 9-10, gy=5-11)
    for py in range(5, 12):
        p[(0, py)] = chitin; p[(1, py)] = chitin
        p[(9, py)] = chitin; p[(10, py)] = chitin
    return w, h, p

def skin_boss_mephisto():
    """Mephisto — ghostly blue-white specter, hollow eyes. Skeleton mesh 7x16."""
    w, h = 7, 16
    p = {}
    ghost  = (160, 190, 220, 255)
    pale   = (200, 220, 240, 255)
    dark   = (60, 80, 120, 255)
    eye    = (40, 200, 255, 255)
    void_c = (20, 30, 60, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = ghost
    # Pale skull
    for px in range(1, 6):
        for py in range(13, 16):
            p[(px, py)] = pale
    # Hollow glowing eyes
    p[(2, 14)] = eye; p[(4, 14)] = eye
    # Dark robes/lower body
    for py in range(0, 8):
        for px in range(w):
            p[(px, py)] = dark
    # Spectral glow streaks
    p[(3, 10)] = pale; p[(3, 11)] = pale; p[(3, 12)] = pale
    # Void patches
    p[(1, 5)] = void_c; p[(5, 6)] = void_c; p[(2, 3)] = void_c
    return w, h, p

def skin_boss_baal():
    """Baal — golden-brown armored demon lord, glowing orange runes. Butcher mesh 11x21."""
    w, h = 12, 21
    p = {}
    armor  = (160, 130, 60, 255)
    gold   = (220, 190, 80, 255)
    rune   = (255, 160, 40, 255)
    dark   = (80, 60, 30, 255)
    skin   = (140, 110, 70, 255)
    eye    = (255, 200, 50, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = skin
    # Armor plates on torso
    for py in range(8, 16):
        for px in range(2, 9):
            p[(px, py)] = armor
    # Gold trim
    for px in range(1, 10):
        p[(px, 15)] = gold; p[(px, 8)] = gold
    # Glowing runes on chest
    p[(4, 12)] = rune; p[(6, 12)] = rune
    p[(5, 11)] = rune; p[(5, 13)] = rune
    # Dark legs
    for py in range(0, 6):
        for px in range(w):
            p[(px, py)] = dark
    # Eyes
    p[(4, 18)] = eye; p[(7, 18)] = eye
    # Horns
    p[(3, 20)] = gold; p[(7, 20)] = gold
    return w, h, p

def skin_boss_diablo():
    """Diablo — dark red scales, black horns, fiery orange belly. Butcher mesh 11x21."""
    w, h = 12, 21
    p = {}
    scale  = (140, 30, 15, 255)
    dark   = (60, 15, 10, 255)
    fire   = (255, 140, 30, 255)
    horn   = (30, 20, 15, 255)
    eye    = (255, 220, 50, 255)
    belly  = (200, 80, 20, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = scale
    # Fiery belly
    for py in range(8, 14):
        for px in range(3, 8):
            p[(px, py)] = belly
    p[(5, 11)] = fire; p[(4, 10)] = fire; p[(6, 10)] = fire
    # Black horns
    p[(2, 20)] = horn; p[(3, 20)] = horn; p[(7, 20)] = horn; p[(8, 20)] = horn
    p[(2, 19)] = horn; p[(8, 19)] = horn
    # Dark legs and arms
    for py in range(0, 5):
        for px in range(w):
            p[(px, py)] = dark
    for py in range(10, 16):
        p[(0, py)] = dark; p[(1, py)] = dark; p[(9, py)] = dark; p[(10, py)] = dark
    # Glowing eyes
    p[(4, 18)] = eye; p[(7, 18)] = eye
    # Spine ridge
    for py in range(6, 18):
        p[(5, py)] = dark
    return w, h, p

def skin_boss_reaper():
    """Grim Reaper — jet black robes, pale skull face, blue soul glow. Skeleton mesh 7x16."""
    w, h = 7, 16
    p = {}
    robe   = (15, 15, 20, 255)
    skull  = (220, 210, 190, 255)
    soul   = (60, 120, 200, 255)
    eye    = (80, 160, 255, 255)
    shadow = (8, 8, 12, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = robe
    # Pale skull face
    for px in range(1, 6):
        for py in range(13, 16):
            p[(px, py)] = skull
    # Hollow glowing eyes
    p[(2, 14)] = eye; p[(4, 14)] = eye
    # Jaw
    p[(2, 13)] = shadow; p[(3, 13)] = shadow; p[(4, 13)] = shadow
    # Soul glow on chest
    p[(3, 10)] = soul; p[(3, 11)] = soul
    p[(2, 10)] = soul; p[(4, 10)] = soul
    # Deep shadow bottom
    for py in range(0, 4):
        for px in range(w):
            p[(px, py)] = shadow
    return w, h, p


def skin_player_warrior():
    """Player Warrior skin — heavy plate + crimson sash + cape + full helm.

    Mesh grid: gx in [-5, 5] (w=11), gy in [0, 17] (h=18).
    Offset: px = gx + 5, py = gy.

    Cape voxels are uv-overridden by the mesh to sample pixel (10, 17), so
    that one texel carries the crimson cape colour even though those voxels
    physically share gx/gy columns with the plate torso.
    """
    w, h = 11, 18

    # Palette — colours are MUTED enough that the same column showing on
    # adjacent voxel faces still reads sensibly (a single pixel paints every
    # exposed face of every voxel in that gx/gy column).
    plate       = (90,  95, 105, 255)   # dark steel plate (torso/legs)
    plate_hi    = (140, 145, 155, 255)  # lighter steel highlight (pauldrons/helm top)
    plate_mid   = (115, 120, 130, 255)  # mid steel (helm shell)
    sash        = (140,  30,  35, 255)  # deep crimson sash band
    cape        = (130,  25,  30, 255)  # crimson cape — sampled via uv override
    eye_slit    = ( 40,  20,  10, 255)  # muted dark for the carved eye slit
    dark_metal  = ( 45,  45,  55, 255)  # nearly-black boots/gauntlets
    belt        = ( 55,  50,  45, 255)  # leather belt under the sash

    p = {}

    # --- Base fill: everything starts as plate so unused-but-sampled-from-an
    #     adjacent-voxel pixels never read transparent/black. ---
    for py in range(h):
        for px in range(w):
            p[(px, py)] = plate

    # --- Boots / sabatons (gy=0) ---
    for px in range(w):
        p[(px, 0)] = dark_metal

    # --- Greaves + thighs (gy=1..3) — plate steel, already set ---
    # Just a slightly lighter knee-height row for variation.
    for px in range(3, 8):
        p[(px, 3)] = (100, 105, 115, 255)

    # --- Belt row (gy=4) ---
    for px in range(w):
        p[(px, 4)] = belt
    # Buckle dot at the centre — slightly lighter so it reads as a buckle.
    p[(5, 4)] = (120, 110, 70, 255)

    # --- Lower-belt row (gy=5) — still belt leather under the sash ---
    for px in range(w):
        p[(px, 5)] = belt

    # --- Crimson sash band (gy=6) ---
    for px in range(w):
        p[(px, 6)] = sash

    # --- Torso plate (gy=7..10) — mostly already plate, add a chest seam ---
    for py in range(7, 11):
        for px in range(2, 9):
            p[(px, py)] = plate
    # Central plate seam (vertical line, slightly darker) — px=5 is gx=0.
    for py in range(7, 11):
        p[(5, py)] = (75, 80, 90, 255)

    # --- Pauldron row (gy=11..12) — wider, brighter steel on the outer
    #     columns (gx=-5/-4 → px=0/1, gx=4/5 → px=9/10). ---
    for py in range(11, 13):
        for px in range(w):
            p[(px, py)] = plate
        for px in (0, 1, 9, 10):
            p[(px, py)] = plate_hi

    # --- Pauldron top ridge (gy=13) ---
    for px in range(w):
        p[(px, 13)] = plate_hi

    # --- Helm shell (gy=14..16) — mid steel, slightly darker than pauldron ---
    for py in range(14, 17):
        for px in range(w):
            p[(px, py)] = plate_mid

    # Horizontal eye-slit row (gy=15) — gx=-1..1 (px=4..6) is the carved
    # opening. The slit pixels are muted dark; they only show through the
    # front carve-out and on the back face of those inner voxels.
    p[(4, 15)] = eye_slit
    p[(5, 15)] = eye_slit
    p[(6, 15)] = eye_slit

    # Helm crown — top row gy=16 slightly lighter for a sculpted helm dome.
    for px in range(3, 8):
        p[(px, 16)] = plate_hi

    # --- Helm crest (gy=17) — only gx=-1..1 voxels exist, but other pixels
    #     in this row are read via uv_overrides cape (10, 17). ---
    for px in range(w):
        p[(px, 17)] = plate_hi          # helm crest colour
    p[(10, 17)] = cape                  # dedicated cape uv-override target

    # --- Gauntlet columns (gx=-4 / +4 → px=1 / px=9) at gy=3 (gauntlet fist)
    #     — paint nearly-black so the fists look like gloved gauntlets. ---
    p[(1, 3)] = dark_metal
    p[(9, 3)] = dark_metal

    return w, h, p


def skin_player_paladin():
    """Player Paladin skin — white-and-gold plate, winged helm, tabard.

    Mesh grid: gx in [-5, 5] (w=11), gy in [0, 17] (h=18).
    Offset: px = gx + 5, py = gy.

    UV-override targets defined by the mesh:
      - Tabard voxels   -> pixel (10, 17)  [tabard cream]
      - Sunburst centre -> pixel  (0, 17)  [rich gold]
      - Sunburst halo   -> pixel  (0, 16)  [light gold "rays"]
      - Back-of-head helm voxels at the eye row remap to gy=13 (helm shell).
    """
    w, h = 11, 18

    # --- Palette ---------------------------------------------------------
    # Each pixel paints every face of every voxel in that (gx, gy) column,
    # so colours are deliberately readable from any angle.
    plate       = (225, 220, 205, 255)   # bone-white plate (base)
    plate_shade = (170, 165, 150, 255)   # warm grey for sides/armpits
    gold_trim   = (220, 175,  60, 255)   # bright gold (helm wings, edges)
    gold_emblem = (190, 145,  40, 255)   # rich gold (sunburst centre)
    gold_halo   = (235, 200,  90, 255)   # lighter gold (sunburst rays halo)
    tabard      = (235, 225, 200, 255)   # warm cream tabard cloth
    eye_slit    = ( 60,  45,  30, 255)   # warm shadow inside the visor
    boot_white  = (215, 210, 195, 255)   # boots — keep mostly bone-white
    boot_gold   = (220, 175,  60, 255)   # boot top trim (gold edge)
    crest_gold  = (220, 175,  60, 255)   # winged helm crest cap

    p = {}

    # --- Base fill: everything starts as plate so any uv sample is sane.
    for py in range(h):
        for px in range(w):
            p[(px, py)] = plate

    # --- Boots / sabatons (gy=0) — bone-white with a gold top trim row.
    for px in range(w):
        p[(px, 0)] = boot_white
    # Boot-top gold band lives one row up (gy=1) so the boot reads as
    # plate with a gold ankle ring.
    for px in range(w):
        p[(px, 1)] = boot_gold

    # --- Greaves / thighs (gy=2..3) — plate-white, already set; add a
    #     subtle shade row at the knee so the legs aren't a flat slab.
    for px in range(3, 8):
        p[(px, 3)] = plate_shade

    # --- Belt / tassets (gy=4..5) — gold trim band over plate.
    # gy=4 is the tasset row; gy=5 is the under-belt. Use gold across the
    # full row at gy=4 (looks like the gold-trimmed belt of a paladin).
    for px in range(w):
        p[(px, 4)] = gold_trim
    for px in range(w):
        p[(px, 5)] = plate          # under-belt = plate-white again

    # --- Torso plate (gy=6..11) — bone-white, with shaded sides so the
    #     squared chest reads as armour rather than a flat board.
    for py in range(6, 12):
        for px in (3, 7):
            p[(px, py)] = plate_shade   # inner shaded "seam" columns
        for px in (2, 8):
            p[(px, py)] = plate_shade   # arm-pit / side shade

    # --- Pauldron rows (gy=10..12): outer columns (px=0/1 left,
    #     px=9/10 right) are gold-edged plate caps. ---
    for py in range(10, 13):
        for px in (0, 10):
            p[(px, py)] = gold_trim     # outer pauldron rim = gold edge
        for px in (1, 9):
            p[(px, py)] = plate         # inner pauldron body = white plate
    # Pauldron top-cap ridge highlight at gy=13 (single voxel each side).
    p[(1, 13)] = gold_trim
    p[(9, 13)] = gold_trim

    # --- Gorget / collar (gy=12) — gold-trimmed plate ring.
    for px in range(4, 7):
        p[(px, 12)] = gold_trim

    # --- Helm shell (gy=13..16) — bone-white plate, slight shade on the
    #     sides so the helmet reads as a curved shape.
    for py in range(13, 17):
        for px in range(w):
            p[(px, py)] = plate
        # Subtle side shading near the cheek rows.
        p[(2, py)] = plate_shade
        p[(8, py)] = plate_shade

    # Visor slit row (gy=14) — gx=-2..2 (px=3..7) is the carved opening.
    # The front face is discarded by the mesh, so this colour only shows
    # on the back face of the eye voxel; back-of-head voxels at gz=0/1
    # are uv-redirected to gy=13 by the mesh to avoid bleeding.
    for px in range(3, 8):
        p[(px, 14)] = eye_slit

    # --- Winged helm tips (gy=14..15, outer columns) ---
    # Wings occupy gx=-4/-3 (px=1/2) and gx=3/4 (px=8/9) at gy=14, with
    # an upper tip voxel at gy=15. Paint these gold so the wing-flares
    # pop against the white helm.
    for px in (1, 2, 8, 9):
        p[(px, 14)] = gold_trim
    p[(1, 15)] = gold_trim
    p[(9, 15)] = gold_trim

    # --- Helm crest (gy=17) — gold spine. Only gx=-1..1 (px=4..6) and
    #     gx=0 (px=5) voxels exist physically; the rest of this row is
    #     reserved for uv-override pixels (see below). ---
    for px in range(4, 7):
        p[(px, 17)] = crest_gold

    # --- UV-override target pixels ---------------------------------------
    # These pixels are sampled by mesh voxels via uv_overrides; their
    # (gx, gy) grid cells are physically empty so we are free to claim
    # them as palette swatches.
    #
    #   (10, 17) = tabard cream       (mesh override (5, 17))
    #   ( 0, 17) = sunburst centre    (mesh override (-5, 17))
    #   ( 0, 16) = sunburst halo      (mesh override (-5, 16))
    p[(10, 17)] = tabard
    p[( 0, 17)] = gold_emblem
    p[( 0, 16)] = gold_halo

    # --- Gauntlet fists (gy=3, px=1 / px=9) — gold-trimmed plate fists.
    p[(1, 3)] = gold_trim
    p[(9, 3)] = gold_trim

    return w, h, p


def skin_player_rogue():
    """Player Rogue skin — black-and-charcoal hooded leathers + rust knife straps.

    Mesh grid (from ``gen_player_rogue``): gx in [-3, 3] (w=7), gy in [0, 14] (h=15).
    Offset: px = gx + 3, py = gy.

    The colour bands stack vertically because the voxel-mesh UV scheme paints one
    pixel per (gx, gy) column — so each py row defines the colour of every voxel
    at that height. Strap rows at py=7 / py=8 carry the rust-leather bandolier
    colour; py=13 is the eye row with ice-blue eyes at the eye columns and a
    near-black hood-shadow band everywhere else; py=11..12 is the cloth face wrap.
    """
    w, h = 7, 15

    # --- Palette (RGBA) --------------------------------------------------
    # Kept muted because the same pixel is read by every face of every voxel in
    # the column, so saturated colours bleed onto side/back faces.
    leather       = ( 35,  35,  45, 255)   # main near-black charcoal leathers
    leather_hi    = ( 55,  55,  70, 255)   # slate highlight (shoulders, hood edges)
    wrap          = ( 75,  75,  85, 255)   # muted grey-charcoal cloth face wrap
    strap         = (120,  80,  55, 255)   # rust-leather brown chest straps
    eye           = (170, 200, 220, 255)   # ice-blue glint (eyes under hood)
    hood_shadow   = ( 20,  20,  28, 255)   # deeper shadow under the hood brim
    boot          = ( 30,  30,  40, 255)   # very dark with a faint blue tinge
    glove         = ( 45,  45,  55, 255)   # slightly lighter leather at the wrist

    p = {}

    # --- Base fill ------------------------------------------------------
    # Default everything to the main leather colour so columns we don't explicitly
    # paint still read sensibly (rather than transparent / black).
    for py in range(h):
        for px in range(w):
            p[(px, py)] = leather

    # --- Boots (py=0) --------------------------------------------------
    for px in range(w):
        p[(px, 0)] = boot

    # --- Shins / soft boot shaft (py=1) --------------------------------
    for px in range(w):
        p[(px, 1)] = boot

    # --- Thighs / lower legs (py=2..3) — leather pants -----------------
    # py=3 is also the glove row, but only px=1 and px=5 are glove voxels (gloves
    # sit at gx=-2 and gx=+2). The rest of the row is empty grid cells (nothing
    # at that gy/gx). Paint the whole row leather, then override the glove pixels.
    for py in range(2, 4):
        for px in range(w):
            p[(px, py)] = leather
    # Fingerless glove columns at gx=-2 -> px=1 and gx=+2 -> px=5; slightly
    # brighter so the gloves read distinct from the pant legs.
    p[(1, 3)] = glove
    p[(5, 3)] = glove

    # --- Belt (py=4) ---------------------------------------------------
    # Painted as the rust-leather strap colour so it visually ties to the
    # bandoliers higher up.
    for px in range(w):
        p[(px, 4)] = strap

    # --- Pelvis (py=5) -------------------------------------------------
    for px in range(w):
        p[(px, 5)] = leather

    # --- Lower torso (py=6) — leather chestpiece -----------------------
    for px in range(w):
        p[(px, 6)] = leather

    # --- Chest-strap rows (py=7 / py=8) — throwing-knife bandoliers ----
    # Two horizontal bands of rust-leather brown so they pop against the
    # near-black leathers. The two rows at different heights read as crossed
    # diagonal straps from any angle because they sit at different vertical
    # heights of the same 3-wide torso column.
    for px in range(w):
        p[(px, 7)] = strap                    # lower bandolier row
        p[(px, 8)] = strap                    # upper bandolier row

    # --- Shoulders (py=9) — leather highlight --------------------------
    # The shoulder caps sit at gx=-2 / +2 (px=1 / px=5); the torso voxels at
    # py=9 are the upper torso. Paint the whole row in the slate highlight so
    # the shoulder caps catch a brighter light than the chest below.
    for px in range(w):
        p[(px, 9)] = leather_hi

    # --- Neck (py=10) — exposed wrap fabric ----------------------------
    for px in range(w):
        p[(px, 10)] = wrap

    # --- Cloth face wrap (py=11..12) -----------------------------------
    # Two rows of fabric covering chin/cheeks below the eyes. The hood does
    # not extend this low, so this band is visible on the FRONT face of the
    # head as a distinct cloth-wrap colour.
    for py in range(11, 13):
        for px in range(w):
            p[(px, py)] = wrap

    # --- Eye row (py=13) -----------------------------------------------
    # Hood shadow base — the brow voxels above the eyes overhang and cast a
    # deep shadow, so this row is near-black everywhere EXCEPT the two eye
    # columns at px=2 (gx=-1) and px=4 (gx=+1), which glow ice-blue.
    for px in range(w):
        p[(px, 13)] = hood_shadow
    p[(2, 13)] = eye
    p[(4, 13)] = eye

    # --- Hood crown + brim (py=14) -------------------------------------
    # The forward brim and crown of the hood — slightly lighter than the
    # main leathers (slate highlight) so the silhouette of the hood's top
    # edge reads against the body below.
    for px in range(w):
        p[(px, 14)] = leather_hi

    return w, h, p


def skin_player_combat_engineer():
    """Player Combat Engineer skin — hazard-orange exosuit + welder helm + power-pack.

    Mesh grid: gx in [-6, 6] (w=13), gy in [0, 16] (h=17).
    Offset: px = gx + 6, py = gy.

    Power-pack voxels are uv-overridden by the mesh to sample three dedicated
    pixels in the gx=6 column (px=12): (12, 14) gunmetal frame, (12, 13)
    darker clamp, (12, 15) cyan glow vent. Those targets only exist on the
    skin (no body voxel actually lives at gy=12+ on gx=6 except a single
    pauldron ridge voxel at gy=11).
    """
    w, h = 13, 17

    # Palette — industrial workshop colours, all RGBA.
    suit        = (225, 120,  35, 255)   # hazard orange — main suit fill
    suit_shadow = (170,  85,  25, 255)   # darker orange — limbs / shadowed sides
    steel       = ( 70,  75,  80, 255)   # gunmetal — helm, gauntlets, boots, pack
    steel_hi    = (120, 125, 135, 255)   # lighter steel highlight — pauldron tops
    goggles     = (230, 180,  60, 255)   # glowing amber welder goggles
    vent_glow   = ( 80, 200, 230, 255)   # cyan power-pack vent
    black       = ( 30,  30,  35, 255)   # near-black joint seals / boot rubber

    p = {}

    # --- Base fill: suit_shadow so any unsampled pixel reads as a
    #     dim industrial colour rather than transparent/black. ---
    for py in range(h):
        for px in range(w):
            p[(px, py)] = suit_shadow

    # --- Boots (gy=0) — gunmetal stompers with a black rubber sole feel. ---
    for px in range(w):
        p[(px, 0)] = steel
    # Add a black rubber accent on the four leg-column boot pixels so the
    # toe-front voxels show as treaded rubber.
    for px in (4, 5, 7, 8):
        p[(px, 0)] = black

    # --- Legs (gy=1..2) — darker orange shadow, suit's lower piston tubes. ---
    for py in (1, 2):
        for px in range(w):
            p[(px, py)] = suit_shadow
    # Knee joint seal (black band at gy=2) on the leg columns only.
    for px in (4, 5, 7, 8):
        p[(px, 2)] = black

    # --- Hip / fist row (gy=3) — fists are gunmetal gauntlets, hips orange. ---
    for px in range(w):
        p[(px, 3)] = suit
    for px in (1, 2, 10, 11):       # fist columns at gx=-5..-4 and gx=4..5
        p[(px, 3)] = steel

    # --- Hip / forearm rows (gy=4..6) — wide orange hip block + steel
    #     gauntlet columns on the sides. ---
    for py in (4, 5, 6):
        for px in range(w):
            p[(px, py)] = suit
        for px in (1, 2, 10, 11):
            p[(px, py)] = steel
    # Black utility belt across the hip (gy=4) for an industrial harness look.
    for px in range(3, 10):
        p[(px, 4)] = black

    # --- Forearm top row (gy=7) — still gauntlet on the side columns,
    #     suit-orange on the torso columns. ---
    for px in range(w):
        p[(px, 7)] = suit
    for px in (1, 2, 10, 11):       # forearm tops still steel
        p[(px, 7)] = steel

    # --- Torso (gy=8..10) — hazard orange with steel chest plate accent. ---
    for py in (8, 9, 10):
        for px in range(w):
            p[(px, py)] = suit
        # Upper-arm columns (gx=-4 / +4 → px=2/10) are gunmetal.
        p[(2, py)] = steel
        p[(10, py)] = steel
    # Central chest plate (steel) — px=5..7 row gy=9.
    for px in (5, 6, 7):
        p[(5 + 0, 9)] = steel
        p[(6, 9)] = steel
        p[(7, 9)] = steel
    # Hazard "warning" stripe across the chest (gy=10) — alternating black on
    # the center five columns reads as workshop safety tape from any angle.
    for px in (4, 6, 8):
        p[(px, 10)] = black

    # --- Pauldron + upper torso row (gy=11) ---
    # Outer ridge voxels live at gx=-6/+6 (px=0/12) — steel highlight.
    # The dome columns gx=-5..-4 (px=1..2) and gx=4..5 (px=10..11) carry
    # the bulk of the pauldron — steel highlight.
    # Torso columns (px=3..9) stay suit-orange.
    for px in range(w):
        p[(px, 11)] = suit
    for px in (0, 1, 2, 10, 11, 12):
        p[(px, 11)] = steel_hi

    # --- Pauldron top row (gy=12) — outer dome columns gunmetal-light;
    #     torso columns stay orange. ---
    for px in range(w):
        p[(px, 12)] = suit
    for px in (1, 2, 10, 11):
        p[(px, 12)] = steel_hi
    # Power-pack frame pixel — gy=12 column gx=6 (px=12) is one of the pack
    # uv-override targets.  Set the dedicated pack frame colour here too so
    # the gx=6 column reads consistently if any voxel falls back to it.
    p[(12, 12)] = steel

    # --- Neck / gorget row (gy=13) — gunmetal collar; px=12 doubles as the
    #     power-pack CLAMP uv-override target. ---
    for px in range(w):
        p[(px, 13)] = steel
    p[(12, 13)] = steel        # dedicated pack clamp pixel (darker steel)

    # --- Helm body row (gy=14) — gunmetal welder helm shell. ---
    for px in range(w):
        p[(px, 14)] = steel
    p[(12, 14)] = steel        # dedicated pack frame uv-override target

    # --- Goggle eye row (gy=15) — helm steel everywhere except the carved
    #     front band (gx=-1..1 → px=5..7) which shows the glowing amber. ---
    for px in range(w):
        p[(px, 15)] = steel
    for px in (5, 6, 7):
        p[(px, 15)] = goggles
    p[(12, 15)] = vent_glow    # dedicated pack VENT uv-override target

    # --- Helm crown + brim row (gy=16) — lighter steel so the hardhat lip
    #     reads against the helm body below. ---
    for px in range(w):
        p[(px, 16)] = steel_hi

    return w, h, p


def skin_player_marksman():
    """Player Marksman skin — old-west sniper in tan duster + wide-brim hat.

    Mesh grid: gx in [-2, 2] (w=5), gy in [0, 17] (h=18).
    Pixel mapping: px = gx + 2, py = gy.

    Notes on a few non-obvious cells:
      * (px=4, py=17) is the dedicated uv_override target for the scope
        voxel — that grid cell is otherwise unoccupied by the mesh, so
        painting it gunmetal does not affect any other voxel face.
      * py=14 is the head-top row whose front-face voxels were carved
        away to suggest brim shadow. The remaining back voxels read as
        the brim's underside / hat-line — paint them deep brown.
      * py=13 is the eye row. The scope sits over the right eye and
        overrides to (2,17), so the right-eye column itself can stay
        face skin (otherwise its own pixel would be sampled by the
        front-face eye-socket area).
    """
    w, h = 5, 18

    # --- Palette (RGBA, 0..255) ---
    duster      = (155, 120,  80, 255)   # warm tan duster body
    duster_dark = ( 85,  60,  40, 255)   # dark brown duster shadow / coat panels
    hat         = ( 75,  50,  30, 255)   # dark brown leather hat crown + brim
    brim_shadow = ( 40,  25,  15, 255)   # deep brown shadow under brim / hat line
    bandolier   = (115,  75,  45, 255)   # leather strap colour
    brass       = (200, 165,  70, 255)   # cartridge brass accents
    scope       = ( 85,  90,  95, 255)   # gunmetal grey scope-goggle
    face        = (195, 155, 115, 255)   # tanned face skin
    boot        = ( 60,  40,  25, 255)   # dark scuffed boot leather

    p = {}

    # --- Base fill: duster colour so any sampled-but-undefined pixel still
    #     reads as the coat instead of black. ---
    for py in range(h):
        for px in range(w):
            p[(px, py)] = duster

    # --- Boots (py=0..1) ---
    # py=0 covers the full boot at gy=0; py=1 is the small heel riser. Both
    # paint as dark boot leather.
    for px in range(w):
        p[(px, 0)] = boot
        p[(px, 1)] = boot

    # --- Coat hem flare (py=2..3) — slightly darker than the upper duster
    #     so the bottom of the coat reads as shadowed. ---
    for px in range(w):
        p[(px, 2)] = duster_dark
        p[(px, 3)] = duster_dark

    # --- Lower coat / thigh row (py=4..5) — main duster colour. ---
    for px in range(w):
        p[(px, 4)] = duster
        p[(px, 5)] = duster

    # --- Belt accent (py=6) + torso lower (py=7). The belt row paints
    #     darker so the waist reads against the bandolier above. ---
    for px in range(w):
        p[(px, 6)] = duster_dark
        p[(px, 7)] = duster

    # --- Bandolier row (py=8) — leather strap across the chest. ---
    for px in range(w):
        p[(px, 8)] = bandolier
    # Brass cartridge dots scattered across the strap. With a 3-wide chest
    # (px=1..3) we drop two visible cartridges; px=0 and px=4 are arm
    # columns at this gy so they stay strap-leather (looks like the strap
    # passing over the shoulder).
    p[(1, 8)] = brass
    p[(3, 8)] = brass

    # --- Upper torso (py=9..10) — duster main. ---
    for px in range(w):
        p[(px,  9)] = duster
        p[(px, 10)] = duster

    # --- Neck (py=11) — narrow exposed throat read as flesh. ---
    for px in range(w):
        p[(px, 11)] = face

    # --- Chin row (py=12) — face skin. ---
    for px in range(w):
        p[(px, 12)] = face

    # --- Eye row (py=13) — face skin; eye sockets carved on the front
    #     show this colour through. The scope voxel is uv-overridden to
    #     (px=4, py=17) below, so the right-eye column stays face. ---
    for px in range(w):
        p[(px, 13)] = face
    # Dark eye dots — paint the eye columns slightly darker so the carved
    # sockets read as eye-holes. Back-of-head voxels in these columns are
    # uv-remapped to (0,13) on the mesh side to avoid showing this dot on
    # the rear of the skull.
    p[(1, 13)] = brim_shadow   # left eye dot
    p[(3, 13)] = brim_shadow   # right eye dot (sits behind the scope)

    # --- Head top row (py=14) — front carved away in the mesh; remaining
    #     back voxels read as deep shadow / hairline under the brim. ---
    for px in range(w):
        p[(px, 14)] = brim_shadow

    # --- Brim row (py=15) — hat leather. Every face of each brim voxel
    #     (top, sides, underside) shares this one pixel, so a deep
    #     leather brown reads as a heavy brim from any angle. ---
    for px in range(w):
        p[(px, 15)] = hat

    # --- Hat crown (py=16..17) — leather crown. ---
    for px in range(w):
        p[(px, 16)] = hat
        p[(px, 17)] = hat
    # Scope uv-override target: (px=4, py=17) — gunmetal grey. The crown
    # voxels only occupy gx=-1..1 (px=1..3) at gy=17, so px=4 is unused
    # and safe to repurpose for the scope colour.
    p[(4, 17)] = scope

    return w, h, p


def skin_player_tinkerer():
    """Player Tinkerer skin — slate-blue vest + teal undershirt + brass goggles.

    Mesh grid: gx in [-2, 2] (w=5), gy in [0, 14] (h=15).
    Offset: px = gx + 2, py = gy.

    Column layout:
      px=0 (gx=-2): arm column + belt extension; gy=11 = TAN pouch pixel
                    referenced by the mesh's uv_overrides for the two pouches.
      px=1 (gx=-1): left half of body (boot/shin/thigh/hip/vest/face).
      px=2 (gx= 0): centre column — UNDERSHIRT TEAL in the torso rows so the
                    unbuttoned-vest gap shows the undershirt through.
      px=3 (gx= 1): right half of body (mirror of px=1).
      px=4 (gx= 2): arm column + chrome drone perch pixel at gy=11.
    """
    # CRITICAL: brass goggles sit ABOVE the eye row (gy=14 forehead band,
    # eyes carved at gy=13). This is the defining cue that separates the
    # Tinkerer from the Combat Engineer (whose welder goggles are at the
    # eye row).
    w, h = 5, 15

    # Palette (RGBA, 0-255).
    vest        = ( 80, 100, 130, 255)   # slate-blue mechanic's vest
    vest_shadow = ( 50,  65,  90, 255)   # darker vest seam
    undershirt  = ( 55, 130, 130, 255)   # teal undershirt visible in centre
    pants       = (100,  75,  50, 255)   # medium brown leather pants
    belt_strap  = ( 65,  45,  30, 255)   # dark leather belt
    pouch_tan   = (140, 105,  70, 255)   # tan-leather pouch highlight
    brass       = (195, 155,  60, 255)   # brass goggles (forehead band)
    face_skin   = (210, 180, 145, 255)   # light olive face/hand skin
    eye_dark    = ( 50,  30,  15, 255)   # dark warm eye dots
    boot        = ( 80,  60,  40, 255)   # scuffed leather boot
    drone       = (120, 160, 200, 255)   # chrome-blue drone perch
    # Slightly darker face for the mouth column so the carved grin reads.
    mouth_dim   = (150, 115,  90, 255)

    p = {}

    # Default fill — vest_shadow is a quiet neutral so any cell that ends up
    # sampled by an adjacent voxel face reads as a dark seam rather than
    # transparent black.
    for py in range(h):
        for px in range(w):
            p[(px, py)] = vest_shadow

    # --- Boots (gy=0) ---
    for px in range(w):
        p[(px, 0)] = boot

    # --- Pants (gy=1..3) ---
    for py in range(1, 4):
        for px in range(w):
            p[(px, py)] = pants

    # --- Hip row (gy=4) — pants + bare hands at the arm columns ---
    for px in range(w):
        p[(px, 4)] = pants
    p[(0, 4)] = face_skin   # left bare hand at gx=-2
    p[(4, 4)] = face_skin   # right bare hand at gx=+2

    # --- Utility belt (gy=5) — dark leather strap across the whole row.
    #     Pouch voxels (uv-overridden) sample (px=0, py=11) instead, so they
    #     read as a brighter tan against the dark strap.
    for px in range(w):
        p[(px, 5)] = belt_strap

    # --- Torso (gy=6..10) — vest flaps + undershirt centre + vest sleeves ---
    # Centre column (px=2 = gx=0) is teal undershirt visible between the
    # vest flaps. gx=-1/+1 columns are slate-blue vest. Arm columns
    # (px=0/4 = gx=-2/+2) painted vest blue so the sleeves match the vest.
    for py in range(6, 11):
        p[(0, py)] = vest          # left sleeve
        p[(1, py)] = vest          # left vest panel + flap
        p[(2, py)] = undershirt    # centre undershirt strip
        p[(3, py)] = vest          # right vest panel + flap
        p[(4, py)] = vest          # right sleeve

    # Vertical vest seam — slightly darker band on the flap columns at one
    # row (gy=7) so the vest reads as a sewn unbuttoned edge rather than a
    # flat blue slab.
    p[(1, 7)] = vest_shadow
    p[(3, 7)] = vest_shadow

    # --- Row gy=11 ---
    #   px=0: FREE -> TAN POUCH pixel (mesh uv_overrides target).
    #   px=1: FREE (vest_shadow default; no voxel samples it).
    #   px=2: neck voxel -> face skin.
    #   px=3: FREE.
    #   px=4: DRONE PERCH voxel -> chrome blue.
    p[(0, 11)] = pouch_tan
    p[(2, 11)] = face_skin
    p[(4, 11)] = drone

    # --- Head row gy=12 — chin / mouth ---
    for px in range(w):
        p[(px, 12)] = face_skin
    p[(2, 12)] = mouth_dim         # mouth column (centre, gx=0)

    # --- Head row gy=13 — eyes ---
    # Centre column = face skin; gx=-1/+1 columns = DARK EYE so the carved
    # front-face holes at (gx=±1, gy=13, gz=-1) reveal a dark voxel behind.
    # (Side and back faces of the eye columns will also sample this dark
    # pixel — accepted bleed; palette is muted enough that it still reads as
    # a face from oblique angles.)
    for px in range(w):
        p[(px, 13)] = face_skin
    p[(1, 13)] = eye_dark          # left eye  (gx=-1)
    p[(3, 13)] = eye_dark          # right eye (gx=+1)

    # --- Head row gy=14 — FOREHEAD / BRASS GOGGLE BAND ---
    # Entire row is brass so the goggle strap wraps around the head (front
    # goggle plates protruding at gz=-2, side straps, rear band). This is
    # the defining visual cue of the Tinkerer.
    for px in range(w):
        p[(px, 14)] = brass

    return w, h, p


def skin_player_sorcerer():
    """Player Sorcerer skin - deep purple robe + star-blue trim + glowing eyes.

    Mesh grid: gx in [-3, 3] (w=7), gy in [0, 17] (h=18).
    Offset: px = gx + 3, py = gy.

    Palette is deliberately muted on the hood-shadow row so the cyan eye
    pixels read as the focal point. The hood-shadow row paints the *inside*
    of the hood (the gz=0 voxels exposed by carving away gz=-1); the mesh's
    uv_overrides remap the back-of-hood voxels to robe-purple pixels so the
    rear of the hood doesn't read as shadow.
    """
    w, h = 7, 18

    # --- Palette ---------------------------------------------------------
    robe       = ( 70,  40, 110, 255)  # deep purple - main robe/hood fabric
    trim       = ( 90, 130, 200, 255)  # star-blue trim (hem & hood lining)
    sash       = (140,  90, 200, 255)  # brighter blue-purple sash band
    shadow     = ( 20,  15,  35, 255)  # near-black inside the hood
    eye        = (140, 220, 255, 255)  # bright cyan-blue glowing eye

    p = {}

    # --- Base fill: everything starts as robe purple so any unused pixel
    #     sampled by an adjacent voxel still reads as fabric. ---
    for py in range(h):
        for px in range(w):
            p[(px, py)] = robe

    # --- Robe hem (gy=0) - star-blue trim band ---
    # The hem is the visible bottom of the robe; trim it in star-blue to
    # echo the celestial-scholar palette.
    for px in range(w):
        p[(px, 0)] = trim

    # --- Robe skirt + torso (gy=1..6, gy=8..11) - solid robe purple ---
    # Already filled with robe; no further changes needed for these rows.

    # --- Sash band (gy=7) - bright blue-purple ---
    for px in range(w):
        p[(px, 7)] = sash

    # --- Hood shadow row 1 (gy=12) - lower face row, all shadow ---
    # The mesh carved away the entire front face of the hood at gy=12, so
    # this whole row shows the inside of the hood through the opening.
    for px in range(w):
        p[(px, 12)] = shadow

    # --- Hood shadow row 2 (gy=13) - eye row, shadow with two glowing eyes ---
    # The mesh kept only the eye voxels at the front; the rest of this row
    # is the carved interior shadow.
    for px in range(w):
        p[(px, 13)] = shadow
    # Left eye: gx=-1 -> px=2.  Right eye: gx=1 -> px=4.
    # KEEP cyan only on the two eye columns so the side-face leakage reads
    # as a faint spell-light glow rather than as a band of cyan.
    p[(2, 13)] = eye
    p[(4, 13)] = eye

    # --- Hood crown / point (gy=14..17) - robe purple, already filled ---
    # The hood tapers to a single point voxel at the top; keep it the same
    # purple as the rest of the hood for a unified silhouette.

    return w, h, p


def skin_player_wanderer():
    """Player Wanderer skin — tattered traveler's robe + face scarf + arm wraps.

    Mesh grid: gx in [-2, 2] (w=5), gy in [0, 15] (h=16).
    Offset: px = gx + 2, py = gy.

    The mesh remaps the back-of-head eye voxels to pixel (0, 15) for the
    hair colour, and remaps a diagonal satchel-strap stripe (plus the
    pouch voxel) to pixel (0, 0) — that cell is otherwise unsampled
    because the gx=-2 column has no voxel at gy=0 (only the gx=-1/+1 leg
    columns reach the floor).
    """
    w, h = 5, 16

    # Palette — muted earth tones. Robe and arm wraps are deliberately close
    # in value because they're meant to read as the same dusty linen.
    robe_main   = (225, 210, 185, 255)   # off-white linen
    robe_shadow = (170, 155, 130, 255)   # tan-grey under-shadow
    robe_dust   = (130, 105,  80, 255)   # dusty brown — bottommost hem row
    wrap        = (210, 195, 170, 255)   # arm wraps (forearm-to-elbow)
    scarf       = (140, 135, 125, 255)   # dust-grey face scarf
    skin_tan    = (190, 160, 130, 255)   # bare tanned skin
    hair        = ( 60,  45,  30, 255)   # dark brown hair / scalp
    eye_dark    = ( 45,  35,  25, 255)   # eyes — warm dark
    strap       = ( 95,  65,  40, 255)   # leather satchel strap + pouch

    p = {}

    # --- Base fill: everything starts as bare tanned skin so any pixel cell
    #     that happens to be sampled but not explicitly set still reads as a
    #     coherent warm tone. ---
    for py in range(h):
        for px in range(w):
            p[(px, py)] = skin_tan

    # --- py=0: foot row + leather-strap override pixel ---
    # (px=0, py=0) is the UV-override target for the satchel strap and
    # pouch — make it explicitly leather brown. The (px=1) and (px=3)
    # cells carry the bare lower-leg voxels; (px=2) and (px=4) are unused
    # at this row.
    p[(0, 0)] = strap                     # dedicated strap pixel
    p[(1, 0)] = skin_tan                  # left foot
    p[(2, 0)] = skin_tan                  # unsampled — neutral
    p[(3, 0)] = skin_tan                  # right foot
    p[(4, 0)] = skin_tan                  # unsampled — neutral

    # --- py=1: lower shin (bare skin) ---
    for px in range(w):
        p[(px, 1)] = skin_tan

    # --- py=2: ragged hem long-tatter row (only px=1 and px=3 are filled) ---
    # The bottommost robe row is dusty brown so the tatters read filthy
    # from travel.
    for px in range(w):
        p[(px, 2)] = robe_dust

    # --- py=3: hem full row + hand fists ---
    # px=0 and px=4 are HAND fists (bare skin); px=1..3 are inner hem
    # fabric rendered in dusty brown.
    p[(0, 3)] = skin_tan                  # left fist
    p[(1, 3)] = robe_dust                 # hem inner left
    p[(2, 3)] = robe_dust                 # hem center
    p[(3, 3)] = robe_dust                 # hem inner right
    p[(4, 3)] = skin_tan                  # right fist

    # --- py=4..6: lower robe body + arm wraps ---
    # px=0 and px=4 share their column between the lower robe (gx=-2 only)
    # and the arm forearm: paint them as wrap fabric so the forearm reads
    # as the wrap band. px=1..3 are the inner robe.
    for py in range(4, 7):
        p[(0, py)] = wrap
        p[(1, py)] = robe_main
        p[(2, py)] = robe_main
        p[(3, py)] = robe_main
        p[(4, py)] = wrap

    # --- py=7: top of the lower robe / forearm-to-elbow transition ---
    # robe_shadow reads as a soft cinch line just under the slim torso.
    for px in range(w):
        p[(px, 7)] = robe_shadow

    # --- py=8..10: slim torso (robe top) + bare upper arms ---
    # px=0 and px=4 are the bare upper-arm columns. px=1..3 are torso fabric.
    for py in range(8, 11):
        p[(0, py)] = skin_tan             # bare left upper arm
        p[(1, py)] = robe_main
        p[(2, py)] = robe_main
        p[(3, py)] = robe_main
        p[(4, py)] = skin_tan             # bare right upper arm

    # --- py=11: neck (only px=2 has a voxel) ---
    for px in range(w):
        p[(px, 11)] = skin_tan

    # --- py=12..13: scarf wrapping the lower half of the face ---
    # Two voxel rows tall — covers mid-nose down to chin per the brief.
    for py in (12, 13):
        for px in range(w):
            p[(px, py)] = scarf

    # --- py=14: eye row ---
    # px=1 and px=3 are the visible eye voxels (front face carved); the
    # other head voxels in those columns are uv-remapped to (0, 15) hair,
    # so the eye pixel only shows through the carved holes. px=0, px=2, and
    # px=4 are non-eye head voxels (sides + nose bridge) — paint as bare
    # skin (above the scarf line).
    p[(0, 14)] = skin_tan                 # left temple
    p[(1, 14)] = eye_dark                 # left eye (gx=-1)
    p[(2, 14)] = skin_tan                 # nose bridge
    p[(3, 14)] = eye_dark                 # right eye (gx=+1)
    p[(4, 14)] = skin_tan                 # right temple

    # --- py=15: top of head (hair) — also the back-of-head uv-override target ---
    for px in range(w):
        p[(px, 15)] = hair

    return w, h, p


def skin_player_ranger():
    """Player Ranger skin — mossy-green hooded cloak, leather accents, leaf cape.

    Mesh grid (from gen_player_ranger): gx in [-3, 3] (w=7), gy in [0, 17]
    (h=18). Offset: px = gx + 3, py = gy.

    Special uv-override targets (set up by the mesh):
      * (px=6, py=17) — cape body. The cape voxels at gz=2 sample only
        this texel, so it carries the cloak-green even though those voxels
        share gy with the leather belt row.
      * (px=6, py=16) — leaf-accent (the lowest center fringe drops).
      * (px=6, py=15) — quiver brown (the two arrow-rods on the upper back).
      * Hood-back layer (gz=2, gy=12..15) is also remapped to (px=6, py=17),
        so the back of the hood reads as cloak.
    """
    w, h = 7, 18

    # Palette — chosen so a single pixel reads acceptably on every face of
    # the voxel column. Greens are slightly desaturated forest tones.
    cloak       = ( 80, 110,  60, 255)   # mossy green — main cloak/hood
    cloak_dark  = ( 50,  75,  40, 255)   # darker green — limbs / lower cloak
    leather     = ( 95,  65,  35, 255)   # warm brown — boots/belt/gloves
    boot_light  = (115,  80,  45, 255)   # slightly lighter boot leather
    skin_tan    = (195, 160, 130, 255)   # pale tan — exposed face
    skin_shadow = (165, 130, 100, 255)   # cheek/jaw shadow under the hood
    eye         = ( 50,  85,  45, 255)   # muted forest-green eye
    mouth       = (110,  75,  60, 255)   # muted lip color
    quiver      = (110,  60,  35, 255)   # red-tinged quiver brown
    leaf        = (160, 180,  80, 255)   # yellow-green leaf accent

    p = {}

    # --- Base fill: cloak green everywhere so any unused / accidentally
    # sampled texel still reads as fabric instead of stark black. ---
    for py in range(h):
        for px in range(w):
            p[(px, py)] = cloak

    # --- Boots (py=0) — warm leather brown ---
    for px in range(w):
        p[(px, 0)] = leather
    # Slight highlight on the boot toe columns (gx=-2 / +1 are the shin/boot
    # columns) so the boots don't read as a uniform brown smear.
    p[(1, 0)] = boot_light    # gx=-2
    p[(4, 0)] = boot_light    # gx=+1

    # --- Shin / ankle (py=1) — dark cloak (pant leg) ---
    for px in range(w):
        p[(px, 1)] = cloak_dark

    # --- Thighs (py=2..3) — dark green leggings ---
    for py in (2, 3):
        for px in range(w):
            p[(px, py)] = cloak_dark
    # Gloves (gx=-3 / +3 → px=0 / px=6) at py=3 — leather brown so the
    # hands read as wrapped/gloved against the green sleeves.
    p[(0, 3)] = leather
    p[(6, 3)] = leather

    # --- Hips / lower cloak (py=4) — main cloak green ---
    for px in range(w):
        p[(px, 4)] = cloak

    # --- Belt row (py=5) — leather strip across the waist ---
    for px in range(w):
        p[(px, 5)] = leather
    # Tiny center buckle highlight.
    p[(3, 5)] = (140, 105, 55, 255)

    # --- Waist + torso cloak (py=6..10) — main cloak green ---
    for py in range(6, 11):
        for px in range(w):
            p[(px, py)] = cloak
    # Center vertical seam line (slightly darker) — px=3 is gx=0.
    for py in range(7, 10):
        p[(3, py)] = cloak_dark

    # --- Shoulders + upper-arm seam (py=10..11) — slight highlight on the
    # outer columns where the arms / shoulder caps sit. ---
    for py in (10, 11):
        p[(0, py)] = cloak_dark    # gx=-3 (left arm)
        p[(6, py)] = cloak_dark    # gx=+3 (right arm)

    # --- Neck / chin / cheek-drape band (py=11..12) ---
    # Inner columns (gx=-1..1 → px=2..4) hold the chin/jaw → skin tone.
    # Outer columns (gx=-2, gx=+2 → px=1, px=5) are the hood cheek drape.
    # Edge columns (gx=±3) are arms.
    for py in (11, 12):
        p[(0, py)] = cloak_dark            # left arm column (gy=11 has shoulder cap voxel)
        p[(1, py)] = cloak                  # left hood cheek drape
        p[(2, py)] = skin_shadow            # left jaw/cheek (under-hood shadow)
        p[(3, py)] = skin_tan               # chin center
        p[(4, py)] = skin_shadow            # right jaw/cheek
        p[(5, py)] = cloak                  # right hood cheek drape
        p[(6, py)] = cloak_dark             # right arm column
    # Mouth — px=3 (gx=0) at py=12 reads through the carved mouth hole.
    p[(3, 12)] = mouth

    # --- Face row (py=13) — chin / jaw ---
    p[(0, 13)] = cloak_dark
    p[(1, 13)] = cloak           # left hood side
    p[(2, 13)] = skin_shadow     # left cheek
    p[(3, 13)] = skin_tan        # nose / center
    p[(4, 13)] = skin_shadow     # right cheek
    p[(5, 13)] = cloak           # right hood side
    p[(6, 13)] = cloak_dark

    # --- Eye row (py=14) — exposed face under the low hood ---
    p[(0, 14)] = cloak_dark
    p[(1, 14)] = cloak           # left hood side
    p[(2, 14)] = eye             # left eye (gx=-1 → px=2)
    p[(3, 14)] = skin_tan        # bridge of the nose
    p[(4, 14)] = eye             # right eye (gx=+1 → px=4)
    p[(5, 14)] = cloak           # right hood side
    p[(6, 14)] = cloak_dark

    # --- Forehead / brow band (py=15) ---
    # Under the hood — paint the inner head columns as dark hood shadow
    # so the forehead doesn't glow brighter than the cheeks.
    for px in range(w):
        p[(px, 15)] = cloak_dark
    # Outer hood sides at py=15 stay main cloak so the silhouette pops.
    p[(1, 15)] = cloak
    p[(5, 15)] = cloak

    # --- Hood top (py=16..17) — main cloak green ---
    for py in (16, 17):
        for px in range(w):
            p[(px, py)] = cloak

    # --- Dedicated uv-override texels (top-right corner) ---
    # The mesh's uv_overrides aim cape body voxels at (3, 17) → px = 6,
    # leaf voxels at (3, 16) → px = 6, quiver voxels at (3, 15) → px = 6.
    # Hood-back voxels also point at (6, 17). Repaint those three cells
    # with their dedicated colors (the base-fill already set them to
    # cloak; we only need to set the special ones).
    p[(6, 17)] = cloak       # cape body / hood-back — keep cloak green
    p[(6, 16)] = leaf        # leaf accent
    p[(6, 15)] = quiver      # quiver rod brown

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
    "player_warrior":     ("player_warrior_skin_42.png",     skin_player_warrior),
    "player_paladin":     ("player_paladin_skin_42.png",     skin_player_paladin),
    "player_rogue":       ("player_rogue_skin_42.png",       skin_player_rogue),
    "player_combat_engineer": ("player_combat_engineer_skin_42.png", skin_player_combat_engineer),
    "player_marksman":    ("player_marksman_skin_42.png",    skin_player_marksman),
    "player_tinkerer":    ("player_tinkerer_skin_42.png",    skin_player_tinkerer),
    "player_sorcerer":   ("player_sorcerer_skin_42.png",   skin_player_sorcerer),
    "player_wanderer":    ("player_wanderer_skin_42.png",    skin_player_wanderer),
    "player_ranger":      ("player_ranger_skin_42.png",      skin_player_ranger),
    "butcher":            ("butcher_skin_42.png",            skin_butcher),
    # Bat-rig variants
    "imp":                ("imp_skin_42.png",                skin_imp),
    "catacomb_bat":       ("catacomb_bat_skin_42.png",       skin_catacomb_bat),
    "cavern_bat":         ("cavern_bat_skin_42.png",         skin_cavern_bat),
    "hellforge_bat":      ("hellforge_bat_skin_42.png",      skin_hellforge_bat),
    "void_bat":           ("void_bat_skin_42.png",           skin_void_bat),
    # Spider-rig variants
    "broodmother":        ("broodmother_skin_42.png",        skin_broodmother),
    # Boss skins
    "boss_andariel":      ("boss_andariel_42.png",      skin_andariel),
    "boss_mephisto":      ("boss_mephisto_42.png",      skin_mephisto),
    "boss_baal":          ("boss_baal_42.png",          skin_baal),
    "boss_diablo":        ("boss_diablo_42.png",        skin_diablo),
    "boss_reaper":        ("boss_reaper_42.png",        skin_reaper),
    "boss_lich":          ("boss_lich_42.png",          skin_lich_lord),
    "boss_spider_queen":  ("boss_spider_queen_42.png",  skin_spider_queen),
    "boss_demon_knight":  ("boss_demon_knight_42.png",  skin_demon_knight),
    "boss_arch_mage":     ("boss_arch_mage_42.png",     skin_arch_mage),
    "hellhound":          ("hellhound_skin_42.png",          skin_hellhound),
    "catacomb_spider":    ("catacomb_spider_skin_42.png",    skin_catacomb_spider),
    "cavern_spider":      ("cavern_spider_skin_42.png",      skin_cavern_spider),
    "hellforge_spider":   ("hellforge_spider_skin_42.png",   skin_hellforge_spider),
    "void_spider":        ("void_spider_skin_42.png",        skin_void_spider),
    # Equipment skins (4x4)
    "weapon_melee_tex":      ("weapon_melee_skin_42.png",      skin_weapon_melee_tex),
    "weapon_hitscan_tex":    ("weapon_hitscan_skin_42.png",    skin_weapon_hitscan_tex),
    "weapon_projectile_tex": ("weapon_projectile_skin_42.png", skin_weapon_projectile_tex),
    "weapon_staff_tex":      ("weapon_staff_skin_42.png",      skin_weapon_staff_tex),
    "weapon_molotov_tex":    ("weapon_molotov_skin_42.png",    skin_weapon_molotov_tex),
    "weapon_sword_tex":          ("weapon_sword_tex_42.png",          skin_weapon_sword_tex),
    "weapon_dagger_tex":         ("weapon_dagger_tex_42.png",         skin_weapon_dagger_tex),
    "weapon_axe_tex":            ("weapon_axe_tex_42.png",            skin_weapon_axe_tex),
    "weapon_cleaver_tex":        ("weapon_cleaver_tex_42.png",        skin_weapon_cleaver_tex),
    "weapon_claymore_tex":       ("weapon_claymore_tex_42.png",       skin_weapon_claymore_tex),
    "weapon_pistol_tex":         ("weapon_pistol_tex_42.png",         skin_weapon_pistol_tex),
    "weapon_smg_tex":            ("weapon_smg_tex_42.png",            skin_weapon_smg_tex),
    "weapon_carbine_tex":        ("weapon_carbine_tex_42.png",        skin_weapon_carbine_tex),
    "weapon_revolver_tex":       ("weapon_revolver_tex_42.png",       skin_weapon_revolver_tex),
    "weapon_bow_tex":            ("weapon_bow_tex_42.png",            skin_weapon_bow_tex),
    "weapon_crossbow_tex":       ("weapon_crossbow_tex_42.png",       skin_weapon_crossbow_tex),
    "weapon_throwing_knife_tex": ("weapon_throwing_knife_tex_42.png", skin_weapon_throwing_knife_tex),
    "weapon_wand_tex":           ("weapon_wand_tex_42.png",           skin_weapon_wand_tex),
    "shock_bolt_tex":            ("shock_bolt_tex_42.png",            skin_shock_bolt_tex),
    "turret_tex":                ("turret_tex_42.png",                skin_turret_tex),
    "helmet_plate_tex":      ("helmet_plate_skin_42.png",      skin_helmet_plate_tex),
    "helmet_leather_tex":    ("helmet_leather_skin_42.png",    skin_helmet_leather_tex),
    "armor_plate_tex":       ("armor_plate_skin_42.png",       skin_armor_plate_tex),
    "armor_leather_tex":     ("armor_leather_skin_42.png",     skin_armor_leather_tex),
    "armor_cloth_tex":       ("armor_cloth_skin_42.png",       skin_armor_cloth_tex),
    "boots_plate_tex":       ("boots_plate_skin_42.png",       skin_boots_plate_tex),
    "boots_leather_tex":     ("boots_leather_skin_42.png",     skin_boots_leather_tex),
    "ring_bone_tex":         ("ring_bone_skin_42.png",         skin_ring_bone_tex),
    "ring_gold_tex":         ("ring_gold_skin_42.png",         skin_ring_gold_tex),
    "shield_wood_tex":       ("shield_wood_skin_42.png",       skin_shield_wood_tex),
    "shield_plate_tex":      ("shield_plate_skin_42.png",      skin_shield_plate_tex),
    # Legendary equipment skins (4x4)
    "legendary_weapon_tex":  ("legendary_weapon_skin_42.png",  skin_legendary_weapon_tex),
    "legendary_armor_tex":   ("legendary_armor_skin_42.png",   skin_legendary_armor_tex),
    "legendary_helm_tex":    ("legendary_helm_skin_42.png",    skin_legendary_helm_tex),
    "legendary_boots_tex":   ("legendary_boots_skin_42.png",   skin_legendary_boots_tex),
    "legendary_ring_tex":    ("legendary_ring_skin_42.png",    skin_legendary_ring_tex),
    "legendary_shield_tex":  ("legendary_shield_skin_42.png",  skin_legendary_shield_tex),
    # Minion portrait icons (8x8)
    "icon_drone":         ("icon_drone_42.png",              skin_icon_drone),
    "icon_swarm":         ("icon_swarm_42.png",              skin_icon_swarm),
    "icon_turret":        ("icon_turret_42.png",             skin_icon_turret),
    "bat_wing":           ("bat_wing_skin_42.png",           skin_bat_wing),
    # Status effect icons (4x4)
    "status_poison":      ("status_poison_42.png",           skin_status_poison),
    "status_burn":        ("status_burn_42.png",             skin_status_burn),
    "status_freeze":      ("status_freeze_42.png",           skin_status_freeze),
    "status_slow":        ("status_slow_42.png",             skin_status_slow),
    "status_invuln":      ("status_invuln_42.png",           skin_status_invuln),
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
    # New archetype enemy skins
    "gargoyle":           ("gargoyle_skin_42.png",           skin_gargoyle),
    "necromancer":        ("necromancer_skin_42.png",        skin_necromancer),
    "cavern_shaman":      ("cavern_shaman_skin_42.png",     skin_cavern_shaman),
    "cavern_herald":      ("cavern_herald_skin_42.png",      skin_cavern_herald),
    "crypt_herald":       ("crypt_herald_skin_42.png",       skin_crypt_herald),
    "sniper_imp":         ("sniper_imp_skin_42.png",         skin_sniper_imp),
    "infernal_herald":    ("infernal_herald_skin_42.png",    skin_infernal_herald),
    "void_necromancer":   ("void_necromancer_skin_42.png",   skin_void_necromancer),
    "void_shaman":        ("void_shaman_skin_42.png",       skin_void_shaman),
    "void_herald":        ("void_herald_skin_42.png",       skin_void_herald),
    # Additional enemy skins
    "bone_archer":         ("bone_archer_skin_42.png",         skin_bone_archer),
    "catacomb_sentinel":   ("catacomb_sentinel_skin_42.png",   skin_catacomb_sentinel),
    "tomb_wraith":         ("tomb_wraith_skin_42.png",         skin_tomb_wraith),
    "plague_bat":          ("plague_bat_skin_42.png",           skin_plague_bat),
    "web_spinner":         ("web_spinner_skin_42.png",         skin_web_spinner),
    "burrowing_widow":     ("burrowing_widow_skin_42.png",     skin_burrowing_widow),
    "cave_troll":          ("cave_troll_skin_42.png",           skin_cave_troll),
    "pit_fiend":           ("pit_fiend_skin_42.png",           skin_pit_fiend),
    "hellforge_smith":     ("hellforge_smith_skin_42.png",     skin_hellforge_smith),
    "succubus":            ("succubus_skin_42.png",             skin_succubus),
    "entropy_weaver":      ("entropy_weaver_skin_42.png",       skin_entropy_weaver),
    "nullifier":           ("nullifier_skin_42.png",           skin_nullifier),
    "mind_flayer":         ("mind_flayer_skin_42.png",         skin_mind_flayer),
    "phase_ripper":        ("phase_ripper_skin_42.png",        skin_phase_ripper),
    "abyssal_titan":       ("abyssal_titan_skin_42.png",       skin_abyssal_titan),
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
