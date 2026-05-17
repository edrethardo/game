#!/usr/bin/env python3
"""Generate 32x32 skill icon pixel art as C++ header data.

Each icon is a 32x32 array of palette indices (0=transparent, 1-4=colors).
Output: src/renderer/skill_icons_data.h

Icons are drawn procedurally using simple geometric primitives (circles, lines,
rectangles, triangles) to create recognizable silhouettes at 32px resolution.
"""

import math
import os
import sys

W, H = 32, 32

def new_icon():
    return [[0]*W for _ in range(H)]

def set_px(icon, x, y, c):
    if 0 <= x < W and 0 <= y < H:
        icon[y][x] = c

def fill_circle(icon, cx, cy, r, c):
    for y in range(H):
        for x in range(W):
            if (x - cx)**2 + (y - cy)**2 <= r**2:
                set_px(icon, x, y, c)

def draw_circle(icon, cx, cy, r, c, thickness=1):
    for y in range(H):
        for x in range(W):
            d = math.sqrt((x - cx)**2 + (y - cy)**2)
            if abs(d - r) < thickness:
                set_px(icon, x, y, c)

def draw_line(icon, x0, y0, x1, y1, c, thickness=1):
    dx = x1 - x0
    dy = y1 - y0
    steps = max(abs(dx), abs(dy), 1)
    for i in range(steps + 1):
        t = i / steps
        x = x0 + dx * t
        y = y0 + dy * t
        for tx in range(-thickness+1, thickness):
            for ty in range(-thickness+1, thickness):
                set_px(icon, int(x+tx), int(y+ty), c)

def fill_rect(icon, x0, y0, x1, y1, c):
    for y in range(max(0,y0), min(H,y1+1)):
        for x in range(max(0,x0), min(W,x1+1)):
            set_px(icon, x, y, c)

def draw_rect(icon, x0, y0, x1, y1, c):
    for x in range(x0, x1+1):
        set_px(icon, x, y0, c); set_px(icon, x, y1, c)
    for y in range(y0, y1+1):
        set_px(icon, x0, y, c); set_px(icon, x1, y, c)

def fill_diamond(icon, cx, cy, r, c):
    for y in range(H):
        for x in range(W):
            if abs(x - cx) + abs(y - cy) <= r:
                set_px(icon, x, y, c)

# --- PALADIN ICONS (gold theme) ---

def icon_holy_smite():
    """Sword with golden flash lines radiating from tip."""
    ic = new_icon()
    # Diagonal sword blade
    for i in range(18):
        draw_line(ic, 6+i, 25-i, 7+i, 26-i, 1)
    # Blade highlight
    for i in range(16):
        set_px(ic, 7+i, 25-i, 4)
    # Crossguard
    draw_line(ic, 9, 17, 17, 21, 2, 2)
    # Handle
    draw_line(ic, 4, 27, 8, 23, 3, 2)
    # Flash lines from tip
    draw_line(ic, 24, 7, 28, 3, 4)
    draw_line(ic, 24, 7, 30, 7, 4)
    draw_line(ic, 24, 7, 26, 2, 4)
    draw_line(ic, 24, 7, 29, 10, 4)
    return ic

def icon_holy_bombardment():
    """3 golden pillars descending from top."""
    ic = new_icon()
    # 3 pillars - vertical beams of light
    for px in [8, 16, 24]:
        fill_rect(ic, px-2, 2, px+1, 24, 1)
        fill_rect(ic, px-1, 0, px, 24, 4)
        # Ground impact circle
        draw_circle(ic, px, 26, 3, 2)
        fill_circle(ic, px, 26, 1, 4)
    # Top bar connecting pillars
    draw_line(ic, 6, 2, 26, 2, 2, 1)
    return ic

def icon_holy_nova():
    """Concentric expanding rings from center."""
    ic = new_icon()
    cx, cy = 16, 16
    # Outer ring
    draw_circle(ic, cx, cy, 13, 3, 1.5)
    # Middle ring
    draw_circle(ic, cx, cy, 9, 1, 1.5)
    # Inner ring
    draw_circle(ic, cx, cy, 5, 4, 1.5)
    # Center bright core
    fill_circle(ic, cx, cy, 2, 4)
    # 8 radial rays
    for a in range(8):
        angle = a * math.pi / 4
        x1 = cx + int(6 * math.cos(angle))
        y1 = cy + int(6 * math.sin(angle))
        x2 = cx + int(13 * math.cos(angle))
        y2 = cy + int(13 * math.sin(angle))
        draw_line(ic, x1, y1, x2, y2, 2)
    return ic

def icon_divine_judgment():
    """Large radiant cross with halo."""
    ic = new_icon()
    cx, cy = 16, 16
    # Vertical bar
    fill_rect(ic, 14, 3, 17, 29, 1)
    # Horizontal bar
    fill_rect(ic, 4, 14, 27, 17, 1)
    # Cross highlight center
    fill_rect(ic, 15, 13, 16, 18, 4)
    fill_rect(ic, 13, 15, 18, 16, 4)
    # Corner rays
    draw_line(ic, 5, 5, 10, 10, 2); draw_line(ic, 27, 5, 22, 10, 2)
    draw_line(ic, 5, 27, 10, 22, 2); draw_line(ic, 27, 27, 22, 22, 2)
    # Halo arc at top
    for a in range(180):
        angle = math.radians(a)
        x = cx + int(8 * math.cos(angle))
        y = 4 - int(3 * math.sin(angle))
        set_px(ic, x, y, 4)
    return ic

# --- MARKSMAN ICONS (amber theme) ---

def icon_aimed_shot():
    """Arrow piercing through 2 vertical bars."""
    ic = new_icon()
    # Arrow shaft - horizontal line
    draw_line(ic, 2, 16, 29, 16, 1, 1)
    # Arrowhead
    draw_line(ic, 27, 16, 30, 13, 4); draw_line(ic, 27, 16, 30, 19, 4)
    set_px(ic, 29, 14, 4); set_px(ic, 29, 18, 4)
    # Fletching
    draw_line(ic, 2, 16, 5, 13, 2); draw_line(ic, 2, 16, 5, 19, 2)
    # Two vertical bars (enemies being pierced)
    fill_rect(ic, 12, 8, 14, 24, 3)
    fill_rect(ic, 21, 8, 23, 24, 3)
    # Pierce holes
    fill_rect(ic, 12, 15, 14, 17, 0)
    fill_rect(ic, 21, 15, 23, 17, 0)
    # Trail line
    draw_line(ic, 2, 15, 10, 15, 2)
    return ic

def icon_explosive_round():
    """Bullet with radial explosion."""
    ic = new_icon()
    cx, cy = 16, 16
    # Central bright core
    fill_circle(ic, cx, cy, 4, 4)
    fill_circle(ic, cx, cy, 2, 1)
    # Explosion rays
    for a in range(12):
        angle = a * math.pi / 6
        x1 = cx + int(5 * math.cos(angle))
        y1 = cy + int(5 * math.sin(angle))
        x2 = cx + int(12 * math.cos(angle))
        y2 = cy + int(12 * math.sin(angle))
        draw_line(ic, x1, y1, x2, y2, 1 if a % 2 == 0 else 2)
    # Outer sparks
    for a in range(6):
        angle = a * math.pi / 3 + 0.3
        x = cx + int(14 * math.cos(angle))
        y = cy + int(14 * math.sin(angle))
        set_px(ic, x, y, 4)
    return ic

def icon_overcharged_magazine():
    """Magazine rectangle with lightning bolt."""
    ic = new_icon()
    # Magazine body
    draw_rect(ic, 8, 4, 23, 27, 2)
    fill_rect(ic, 9, 5, 22, 26, 3)
    # Ammo lines inside
    for y in range(7, 25, 3):
        draw_line(ic, 10, y, 21, y, 2)
    # Lightning bolt overlay
    draw_line(ic, 18, 6, 13, 15, 4, 2)
    draw_line(ic, 13, 15, 19, 15, 4, 2)
    draw_line(ic, 19, 15, 14, 25, 4, 2)
    # Glow at top
    set_px(ic, 18, 5, 1); set_px(ic, 19, 5, 1)
    return ic

def icon_headshot():
    """Crosshair with skull center."""
    ic = new_icon()
    cx, cy = 16, 16
    # Outer crosshair ring
    draw_circle(ic, cx, cy, 12, 2, 1.2)
    # Cross lines
    draw_line(ic, cx, 1, cx, 7, 1)
    draw_line(ic, cx, 25, cx, 31, 1)
    draw_line(ic, 1, cy, 7, cy, 1)
    draw_line(ic, 25, cy, 31, cy, 1)
    # Inner skull outline
    fill_circle(ic, cx, cy-1, 5, 1)
    fill_rect(ic, 13, 19, 19, 22, 1)
    # Eyes
    fill_rect(ic, 13, 13, 14, 15, 0)
    fill_rect(ic, 18, 13, 19, 15, 0)
    # Mouth/teeth
    for x in [13, 15, 17, 19]:
        set_px(ic, x, 20, 0)
    # Highlight
    set_px(ic, cx, cy-4, 4)
    return ic

# --- TINKERER ICONS (cyan theme) ---

def icon_swarm_deploy():
    """Cloud of scattered diamond shapes."""
    ic = new_icon()
    positions = [(8,6), (20,4), (5,14), (16,10), (26,12),
                 (10,20), (22,18), (14,26), (28,24), (4,28)]
    for i, (x, y) in enumerate(positions):
        r = 2 if i < 4 else 1
        c = 4 if i < 3 else (1 if i < 7 else 2)
        fill_diamond(ic, x, y, r, c)
    return ic

def icon_overclock():
    """Gear cog with upward arrow."""
    ic = new_icon()
    cx, cy = 16, 16
    # Gear body
    fill_circle(ic, cx, cy, 8, 2)
    fill_circle(ic, cx, cy, 5, 3)
    # Gear teeth
    for a in range(8):
        angle = a * math.pi / 4
        x = cx + int(10 * math.cos(angle))
        y = cy + int(10 * math.sin(angle))
        fill_rect(ic, x-1, y-1, x+1, y+1, 2)
    # Upward arrow in center
    draw_line(ic, cx, 10, cx, 22, 4, 1)
    draw_line(ic, cx, 10, cx-3, 14, 4)
    draw_line(ic, cx, 10, cx+3, 14, 4)
    return ic

def icon_detonate_swarm():
    """Central burst with drone dots at tips."""
    ic = new_icon()
    cx, cy = 16, 16
    # Central explosion core
    fill_circle(ic, cx, cy, 3, 4)
    fill_circle(ic, cx, cy, 1, 1)
    # 6 radial explosion lines with dots
    for a in range(6):
        angle = a * math.pi / 3
        x1 = cx + int(4 * math.cos(angle))
        y1 = cy + int(4 * math.sin(angle))
        x2 = cx + int(12 * math.cos(angle))
        y2 = cy + int(12 * math.sin(angle))
        draw_line(ic, x1, y1, x2, y2, 1)
        # Drone dot at tip
        fill_circle(ic, x2, y2, 2, 2)
        set_px(ic, x2, y2, 4)
    return ic

def icon_swarm_queen():
    """Large hexagonal queen with orbiting dots."""
    ic = new_icon()
    cx, cy = 16, 16
    # Queen body - large diamond
    fill_diamond(ic, cx, cy, 8, 2)
    fill_diamond(ic, cx, cy, 5, 1)
    # Crown on top
    draw_line(ic, cx-3, 7, cx, 4, 4)
    draw_line(ic, cx+3, 7, cx, 4, 4)
    set_px(ic, cx, 3, 4); set_px(ic, cx-2, 5, 4); set_px(ic, cx+2, 5, 4)
    # Orbiting mini drones
    for a, r in [(0.5, 13), (1.5, 12), (3.0, 13), (4.5, 12)]:
        x = cx + int(r * math.cos(a))
        y = cy + int(r * math.sin(a))
        fill_diamond(ic, x, y, 1, 4)
    return ic

# --- ROGUE ICONS (purple theme) ---

def icon_fan_of_knives():
    """5 blades radiating outward in a fan."""
    ic = new_icon()
    cx, cy = 16, 22
    for a in range(5):
        angle = math.radians(-120 + a * 30)
        x2 = cx + int(16 * math.cos(angle))
        y2 = cy + int(16 * math.sin(angle))
        draw_line(ic, cx, cy, x2, y2, 1 if a % 2 == 0 else 2, 1)
        # Blade tip highlight
        set_px(ic, x2, y2, 4)
        x3 = cx + int(14 * math.cos(angle))
        y3 = cy + int(14 * math.sin(angle))
        set_px(ic, x3, y3, 4)
    # Central grip
    fill_circle(ic, cx, cy, 3, 3)
    fill_circle(ic, cx, cy, 1, 4)
    return ic

def icon_shadow_step():
    """Boot/footprint with motion trail."""
    ic = new_icon()
    # Boot shape (right foot, side view)
    fill_rect(ic, 14, 8, 22, 24, 1)   # main boot
    fill_rect(ic, 22, 20, 26, 24, 1)  # toe
    fill_rect(ic, 14, 24, 26, 26, 2)  # sole
    fill_rect(ic, 15, 9, 21, 12, 4)   # boot top highlight
    # Motion trail lines (left side, ghosting effect)
    draw_line(ic, 10, 10, 10, 22, 3)
    draw_line(ic, 7, 12, 7, 20, 3)
    draw_line(ic, 4, 14, 4, 18, 3)
    return ic

def icon_shadow_dance():
    """Cloaked figure with swirl lines."""
    ic = new_icon()
    cx = 16
    # Hooded head
    fill_circle(ic, cx, 8, 4, 1)
    fill_circle(ic, cx, 7, 2, 4)
    # Cloak body (triangle)
    for y in range(12, 28):
        w = (y - 12) * 8 // 16
        fill_rect(ic, cx-w, y, cx+w, y, 2 if y < 20 else 3)
    # Swirl/motion lines
    for i, offset in enumerate([(-8, 6), (-10, 12), (-7, 18), (8, 8), (10, 14)]):
        c = 4 if i < 2 else 1
        draw_line(ic, cx+offset[0], offset[1], cx+offset[0]-3, offset[1]-1, c)
    return ic

# --- RANGER ICONS (green/brown theme) ---

def icon_volley():
    """5 arrows raining downward."""
    ic = new_icon()
    for i, (x, startY) in enumerate([(6,4), (12,2), (16,0), (20,3), (26,5)]):
        # Arrow shaft - angled downward
        endY = startY + 18
        draw_line(ic, x, startY, x-2, endY, 1)
        # Arrowhead
        draw_line(ic, x-2, endY, x-4, endY-3, 4)
        draw_line(ic, x-2, endY, x, endY-3, 4)
        # Fletching
        set_px(ic, x+1, startY+1, 2)
        set_px(ic, x-1, startY+1, 2)
    # Ground impact zone (bottom)
    draw_line(ic, 2, 28, 29, 28, 3)
    return ic

def icon_piercing_shot():
    """Single arrow through 3 circles."""
    ic = new_icon()
    # Arrow - thick horizontal line
    draw_line(ic, 2, 16, 29, 16, 1, 1)
    draw_line(ic, 2, 15, 29, 15, 4)
    # Arrowhead
    draw_line(ic, 28, 16, 31, 13, 4); draw_line(ic, 28, 16, 31, 19, 4)
    # 3 enemy circles being pierced
    for cx in [10, 18, 26]:
        draw_circle(ic, cx, 16, 4, 2, 1.2)
        # Pierce gap
        fill_rect(ic, cx-1, 14, cx+1, 18, 0)
        draw_line(ic, cx-1, 16, cx+1, 16, 1)
    # Fletching
    draw_line(ic, 2, 16, 5, 13, 3)
    draw_line(ic, 2, 16, 5, 19, 3)
    return ic

def icon_barrage():
    """Tight cluster of horizontal arrows."""
    ic = new_icon()
    offsets = [(-2, 6), (0, 10), (1, 14), (0, 18), (-1, 22), (2, 26)]
    for dx, y in offsets:
        # Arrow shaft
        draw_line(ic, 4+dx, y, 24+dx, y, 1)
        # Arrowhead
        draw_line(ic, 24+dx, y, 27+dx, y-2, 4)
        draw_line(ic, 24+dx, y, 27+dx, y+2, 4)
        # Fletching
        set_px(ic, 4+dx, y-1, 2)
        set_px(ic, 4+dx, y+1, 2)
    return ic

def icon_mark_prey():
    """Bullseye target with crosshair."""
    ic = new_icon()
    cx, cy = 16, 16
    # 3 concentric circles
    draw_circle(ic, cx, cy, 13, 3, 1.2)
    draw_circle(ic, cx, cy, 9, 2, 1.2)
    draw_circle(ic, cx, cy, 5, 1, 1.2)
    # Center dot
    fill_circle(ic, cx, cy, 2, 4)
    # Crosshair lines
    draw_line(ic, cx, 1, cx, 6, 1)
    draw_line(ic, cx, 26, cx, 31, 1)
    draw_line(ic, 1, cy, 6, cy, 1)
    draw_line(ic, 26, cy, 31, cy, 1)
    return ic

# --- UPGRADED LEGENDARY ICONS ---

def icon_frozen_orb():
    """Ice orb with crystal shards."""
    ic = new_icon()
    cx, cy = 16, 16
    fill_circle(ic, cx, cy, 8, 1)
    draw_circle(ic, cx, cy, 8, 2, 1.5)
    fill_circle(ic, cx, cy, 4, 4)
    # Crystal shards radiating
    for a in range(6):
        angle = a * math.pi / 3
        x = cx + int(11 * math.cos(angle))
        y = cy + int(11 * math.sin(angle))
        fill_diamond(ic, x, y, 2, 4)
    draw_circle(ic, cx, cy, 11, 3)
    return ic

def icon_chain_lightning():
    """Zigzag lightning bolt."""
    ic = new_icon()
    points = [(8,3), (18,8), (10,13), (22,18), (12,23), (20,28)]
    for i in range(len(points)-1):
        draw_line(ic, points[i][0], points[i][1],
                  points[i+1][0], points[i+1][1], 1, 2)
    # Highlight center of bolt
    for i in range(len(points)-1):
        draw_line(ic, points[i][0], points[i][1],
                  points[i+1][0], points[i+1][1], 4, 1)
    # Spark dots
    for x, y in [(5, 5), (24, 10), (6, 20), (26, 25)]:
        set_px(ic, x, y, 4)
    return ic

def icon_meteor_strike():
    """Meteor with fire trail."""
    ic = new_icon()
    # Meteor rock
    fill_circle(ic, 22, 22, 6, 1)
    fill_circle(ic, 22, 22, 3, 4)
    draw_circle(ic, 22, 22, 6, 2)
    # Fire trail
    draw_line(ic, 22, 16, 8, 2, 2, 2)
    draw_line(ic, 22, 16, 10, 4, 4, 1)
    # Impact ring at bottom
    draw_circle(ic, 22, 26, 8, 3)
    return ic

def icon_blood_nova():
    """Blood explosion cross pattern."""
    ic = new_icon()
    cx, cy = 16, 16
    fill_circle(ic, cx, cy, 4, 1)
    fill_circle(ic, cx, cy, 2, 4)
    # 4 cardinal tendrils
    for angle in [0, 90, 180, 270]:
        a = math.radians(angle)
        for d in range(5, 14):
            x = cx + int(d * math.cos(a))
            y = cy + int(d * math.sin(a))
            w = max(1, 3 - d // 5)
            for wx in range(-w+1, w):
                for wy in range(-w+1, w):
                    set_px(ic, x+wx, y+wy, 1 if d < 10 else 2)
    # Droplets at tips
    for angle in [45, 135, 225, 315]:
        a = math.radians(angle)
        x = cx + int(10 * math.cos(a))
        y = cy + int(10 * math.sin(a))
        set_px(ic, x, y, 4)
    return ic

def icon_phase_dash():
    """Diagonal arrow with speed lines."""
    ic = new_icon()
    # Main dash arrow - diagonal
    draw_line(ic, 5, 27, 27, 5, 1, 2)
    draw_line(ic, 6, 27, 27, 6, 4, 1)
    # Arrowhead
    draw_line(ic, 27, 5, 22, 5, 4)
    draw_line(ic, 27, 5, 27, 10, 4)
    # Speed lines (trailing)
    draw_line(ic, 3, 25, 10, 18, 2)
    draw_line(ic, 6, 28, 13, 21, 2)
    draw_line(ic, 1, 22, 7, 16, 3)
    return ic

def icon_arc_fire():
    """Fire arc / flame pattern."""
    ic = new_icon()
    cx = 16
    # Main flame
    for y in range(6, 28):
        fy = (y - 6) / 22.0
        w = int(6 * math.sin(fy * math.pi))
        c = 4 if fy < 0.3 else (1 if fy < 0.7 else 2)
        fill_rect(ic, cx-w, y, cx+w, y, c)
    # Side flames
    for dx, h in [(-5, 12), (5, 14)]:
        for y in range(h, 26):
            fy = (y - h) / (26 - h)
            w = int(3 * math.sin(fy * math.pi))
            fill_rect(ic, cx+dx-w, y, cx+dx+w, y, 2 if fy < 0.5 else 3)
    # Bright tip
    fill_circle(ic, cx, 8, 2, 4)
    return ic


# --- WARRIOR ICONS (red/steel theme) ---

def icon_cleave():
    """Wide axe swing arc."""
    ic = new_icon()
    cx, cy = 16, 20
    for a in range(-60, 61, 2):
        angle = math.radians(a - 90)
        for r in range(8, 15):
            x = cx + int(r * math.cos(angle))
            y = cy + int(r * math.sin(angle))
            c = 4 if r < 10 else (1 if r < 13 else 2)
            set_px(ic, x, y, c)
    fill_circle(ic, cx, cy, 3, 3)
    return ic

def icon_war_cry():
    """Shouting head with sound waves."""
    ic = new_icon()
    fill_circle(ic, 12, 14, 5, 1)
    fill_circle(ic, 12, 14, 3, 4)
    # Sound waves radiating right
    for ring in range(3):
        r = 8 + ring * 3
        for a in range(-40, 41, 3):
            angle = math.radians(a)
            x = 12 + int(r * math.cos(angle))
            y = 14 + int(r * math.sin(angle))
            set_px(ic, x, y, 2 if ring < 2 else 3)
    return ic

def icon_thunderclap():
    """Ground slam shockwave."""
    ic = new_icon()
    cx = 16
    # Fist coming down
    fill_rect(ic, 13, 4, 19, 14, 1)
    fill_rect(ic, 14, 5, 18, 8, 4)
    # Ground crack lines
    draw_line(ic, cx, 18, 4, 28, 2)
    draw_line(ic, cx, 18, 28, 28, 2)
    draw_line(ic, cx, 18, cx-8, 26, 1)
    draw_line(ic, cx, 18, cx+8, 26, 1)
    # Impact point
    fill_circle(ic, cx, 18, 2, 4)
    return ic

def icon_whirlwind():
    """Spinning spiral."""
    ic = new_icon()
    cx, cy = 16, 16
    for i in range(120):
        angle = i * 0.15
        r = 2 + i * 0.1
        x = cx + int(r * math.cos(angle))
        y = cy + int(r * math.sin(angle))
        c = 4 if i < 30 else (1 if i < 70 else 2)
        set_px(ic, x, y, c)
        set_px(ic, x+1, y, c)
    return ic

def icon_earthquake():
    """Cracked ground with debris."""
    ic = new_icon()
    # Ground crack pattern
    draw_line(ic, 4, 14, 28, 18, 1, 2)
    draw_line(ic, 16, 8, 12, 28, 2, 1)
    draw_line(ic, 16, 8, 22, 26, 2, 1)
    # Debris chunks flying up
    for x, y in [(8, 6), (20, 4), (26, 8), (6, 10)]:
        fill_rect(ic, x, y, x+2, y+2, 4)
    # Ground fill
    for y in range(20, 30):
        draw_line(ic, 2, y, 30, y, 3)
    return ic

# --- SORCERER ICONS (fire/arcane theme) ---

def icon_fireball():
    """Flaming orb with trail."""
    ic = new_icon()
    # Fire trail
    draw_line(ic, 4, 18, 16, 16, 2, 2)
    draw_line(ic, 6, 14, 16, 16, 3, 1)
    # Fireball core
    fill_circle(ic, 22, 14, 7, 1)
    fill_circle(ic, 22, 14, 4, 4)
    draw_circle(ic, 22, 14, 7, 2, 1.2)
    # Flame wisps
    draw_line(ic, 22, 7, 24, 3, 4)
    draw_line(ic, 26, 9, 29, 6, 2)
    return ic

# --- COMBAT ENGINEER ICONS (electric/tech theme) ---

def icon_shock_bolt():
    """Electric bolt projectile."""
    ic = new_icon()
    draw_line(ic, 4, 16, 12, 12, 1, 2)
    draw_line(ic, 12, 12, 16, 18, 4, 2)
    draw_line(ic, 16, 18, 24, 10, 1, 2)
    draw_line(ic, 24, 10, 28, 16, 4, 1)
    # Spark dots
    for x, y in [(8, 8), (20, 6), (26, 20), (10, 22)]:
        set_px(ic, x, y, 4)
        set_px(ic, x+1, y, 2)
    return ic

def icon_deploy_turret():
    """Turret silhouette."""
    ic = new_icon()
    # Base/treads
    fill_rect(ic, 8, 22, 24, 28, 3)
    fill_rect(ic, 10, 24, 22, 26, 2)
    # Body
    fill_rect(ic, 11, 14, 21, 22, 1)
    fill_rect(ic, 13, 16, 19, 20, 4)
    # Barrel
    fill_rect(ic, 20, 16, 28, 18, 1)
    fill_rect(ic, 26, 15, 29, 19, 2)
    # Antenna
    draw_line(ic, 16, 14, 16, 8, 2)
    set_px(ic, 16, 7, 4)
    return ic

def icon_tesla_coil():
    """Tesla coil with arcs."""
    ic = new_icon()
    cx = 16
    # Coil body
    fill_rect(ic, 13, 16, 19, 28, 2)
    fill_rect(ic, 14, 18, 18, 26, 3)
    # Top sphere
    fill_circle(ic, cx, 12, 5, 1)
    fill_circle(ic, cx, 12, 3, 4)
    # Lightning arcs
    draw_line(ic, cx-5, 12, 3, 6, 4)
    draw_line(ic, cx+5, 12, 29, 6, 4)
    draw_line(ic, cx-4, 10, 6, 16, 2)
    draw_line(ic, cx+4, 10, 26, 16, 2)
    return ic

def icon_mech_overdrive():
    """Gear with lightning bolt."""
    ic = new_icon()
    cx, cy = 16, 16
    fill_circle(ic, cx, cy, 10, 2)
    fill_circle(ic, cx, cy, 7, 3)
    # Gear teeth
    for a in range(6):
        angle = a * math.pi / 3
        x = cx + int(12 * math.cos(angle))
        y = cy + int(12 * math.sin(angle))
        fill_rect(ic, x-1, y-1, x+1, y+1, 2)
    # Lightning bolt center
    draw_line(ic, 18, 8, 14, 16, 4, 2)
    draw_line(ic, 14, 16, 19, 16, 4, 1)
    draw_line(ic, 19, 16, 15, 24, 4, 2)
    return ic

def icon_poison_cloud():
    """Green toxic cloud."""
    ic = new_icon()
    fill_circle(ic, 12, 16, 7, 1)
    fill_circle(ic, 20, 14, 6, 1)
    fill_circle(ic, 16, 20, 5, 2)
    fill_circle(ic, 14, 15, 3, 4)
    fill_circle(ic, 20, 13, 2, 4)
    # Skull hint in cloud
    set_px(ic, 14, 14, 3); set_px(ic, 18, 14, 3)
    set_px(ic, 16, 16, 3)
    return ic


def write_header(icons, path):
    """Write all icons as C++ static arrays to a header file."""
    with open(path, 'w') as f:
        f.write("// Auto-generated by tools/gen_skill_icons.py — do not edit manually.\n")
        f.write("// 32x32 skill icon pixel art (palette indices 0-4).\n")
        f.write("#pragma once\n\n")
        f.write("#include \"core/types.h\"\n\n")
        f.write("static constexpr u32 SKILL_ICON_SIZE = 32;\n\n")

        for name, data in icons:
            f.write(f"static const u8 kIcon32_{name}[32][32] = {{\n")
            for y in range(32):
                row = ",".join(str(data[y][x]) for x in range(32))
                f.write(f"    {{{row}}},\n")
            f.write("};\n\n")

    print(f"Generated {len(icons)} icons to {path}")


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)
    out_path = os.path.join(repo_root, "src", "renderer", "skill_icons_data.h")

    icons = [
        # Warrior
        ("Cleave",           icon_cleave()),
        ("WarCry",           icon_war_cry()),
        ("Thunderclap",      icon_thunderclap()),
        ("Whirlwind",        icon_whirlwind()),
        ("Earthquake",       icon_earthquake()),
        # Sorcerer
        ("Fireball",         icon_fireball()),
        # Combat Engineer
        ("ShockBolt",        icon_shock_bolt()),
        ("DeployTurret",     icon_deploy_turret()),
        ("TeslaCoil",        icon_tesla_coil()),
        ("MechOverdrive",    icon_mech_overdrive()),
        # Rogue (Smoke Bomb)
        ("PoisonCloud",      icon_poison_cloud()),
        # Paladin
        ("HolySmite",        icon_holy_smite()),
        ("HolyBombardment",  icon_holy_bombardment()),
        ("HolyNova",         icon_holy_nova()),
        ("DivineJudgment",   icon_divine_judgment()),
        # Marksman
        ("AimedShot",        icon_aimed_shot()),
        ("ExplosiveRound",   icon_explosive_round()),
        ("OverchargedMag",   icon_overcharged_magazine()),
        ("Headshot",         icon_headshot()),
        # Tinkerer
        ("SwarmDeploy",      icon_swarm_deploy()),
        ("Overclock",        icon_overclock()),
        ("DetonateSwarm",    icon_detonate_swarm()),
        ("SwarmQueen",       icon_swarm_queen()),
        # Rogue
        ("FanOfKnives",      icon_fan_of_knives()),
        ("ShadowStep",       icon_shadow_step()),
        ("ShadowDance",      icon_shadow_dance()),
        # Ranger
        ("Volley",           icon_volley()),
        ("PiercingShot",     icon_piercing_shot()),
        ("Barrage",          icon_barrage()),
        ("MarkPrey",         icon_mark_prey()),
        # Upgraded legendaries
        ("FrozenOrb",        icon_frozen_orb()),
        ("ChainLightning",   icon_chain_lightning()),
        ("MeteorStrike",     icon_meteor_strike()),
        ("BloodNova",        icon_blood_nova()),
        ("PhaseDash",        icon_phase_dash()),
        ("ArcFire",          icon_arc_fire()),
    ]

    write_header(icons, out_path)


if __name__ == "__main__":
    main()
