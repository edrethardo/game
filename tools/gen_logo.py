#!/usr/bin/env python3
"""Generate the game logo as a pixel-art dungeon entrance scene.

Outputs 3 sizes:
  assets/logo_sm.png  — 256x128  (small/icon)
  assets/logo_md.png  — 512x256  (README)
  assets/logo_lg.png  — 800x400  (store/banner)

Usage:
    python3 tools/gen_logo.py
"""

import math
import os
import random
import sys

try:
    from PIL import Image, ImageDraw, ImageFont, ImageFilter
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)

# Base resolution — downscaled for smaller outputs
BW, BH = 800, 400

random.seed(42)  # deterministic output


def lerp_color(c1, c2, t):
    return tuple(int(c1[i] + (c2[i] - c1[i]) * t) for i in range(len(c1)))


def draw_scene_art(img, with_figure=True):
    """Draw the dungeon entrance scene WITHOUT the title text or vignette, so the art can be
    reused as a text-free background (the Steam capsule tool composites its own wordmark over
    this — see tools/gen_steam_capsules.py). draw_scene() below adds the title + vignette to
    reproduce the original full logo.

    with_figure=True (default) draws the static adventurer silhouette at the top of the stairs,
    matching the original logo. Pass False to omit it — the logo-walk clip (tools/gen_logo_walk.py)
    renders a figure-free scene so it can composite its own animated walker."""
    draw = ImageDraw.Draw(img)
    W, H = img.size

    # --- 1. Background: dark gradient (deep blue-black top → warm at bottom center) ---
    for y in range(H):
        t = y / H
        r = int(8 + t * 15)
        g = int(8 + t * 10)
        b = int(15 + t * 5)
        draw.line([(0, y), (W, y)], fill=(r, g, b, 255))

    # --- 2. Hell glow from below (radial gradient, bottom center) ---
    cx, cy = W // 2, H
    max_r = int(H * 0.7)
    for y in range(H // 3, H):
        for x in range(W // 4, 3 * W // 4):
            dx, dy = x - cx, y - cy
            dist = math.sqrt(dx * dx + dy * dy)
            if dist < max_r:
                t = 1.0 - dist / max_r
                t = t * t  # quadratic falloff
                intensity = t * 0.6
                px = img.getpixel((x, y))
                nr = min(255, int(px[0] + 200 * intensity))
                ng = min(255, int(px[1] + 80 * intensity))
                nb = min(255, int(px[2] + 20 * intensity))
                img.putpixel((x, y), (nr, ng, nb, 255))

    # --- 3. Stone pillars ---
    pillar_w = W // 8
    pillar_h = int(H * 0.75)
    pillar_top = int(H * 0.08)

    for side in [0, 1]:  # left, right
        px = int(W * 0.18) if side == 0 else int(W * 0.82) - pillar_w
        for y in range(pillar_top, pillar_top + pillar_h):
            for x in range(px, px + pillar_w):
                # Stone texture: base gray + noise
                noise = random.randint(-12, 12)
                base = 55 + int((y - pillar_top) / pillar_h * 15)
                g = max(0, min(255, base + noise))
                # Darker edges for depth
                edge_dist = min(x - px, px + pillar_w - x)
                if edge_dist < 4:
                    g = int(g * 0.6)
                img.putpixel((x, y), (g, g, g + 3, 255))

        # Pillar cap (lighter stone block)
        cap_h = int(H * 0.04)
        for y in range(pillar_top - cap_h, pillar_top):
            for x in range(px - 4, px + pillar_w + 4):
                if 0 <= x < W and 0 <= y < H:
                    noise = random.randint(-8, 8)
                    g = max(0, min(255, 70 + noise))
                    img.putpixel((x, y), (g, g, g + 2, 255))

    # --- 4. Stone arch connecting pillars ---
    arch_cx = W // 2
    arch_cy = pillar_top
    arch_rx = int(W * 0.32)
    arch_ry = int(H * 0.18)
    arch_thickness = int(H * 0.06)

    for y in range(arch_cy - arch_ry - arch_thickness, arch_cy + 5):
        for x in range(int(W * 0.18), int(W * 0.82)):
            dx = (x - arch_cx) / arch_rx
            dy = (y - arch_cy) / arch_ry
            dist = math.sqrt(dx * dx + dy * dy)
            # Inside the arch band
            if 0.7 < dist < 1.15 and dy < 0.1:
                noise = random.randint(-10, 10)
                base = 60 + int(dist * 15)
                g = max(0, min(255, base + noise))
                img.putpixel((x, y), (g, g - 2, g - 3, 255))

    # --- 5. Dark interior (inside the archway) ---
    for y in range(arch_cy - arch_ry + arch_thickness, pillar_top + pillar_h):
        for x in range(int(W * 0.18) + pillar_w, int(W * 0.82) - pillar_w):
            dx = (x - arch_cx) / (arch_rx - pillar_w * 0.5)
            dy = (y - arch_cy) / arch_ry
            dist = math.sqrt(dx * dx + dy * dy)
            if dist < 0.95 or y > arch_cy:
                # Dark interior with subtle glow from below
                glow_t = max(0, (y - H * 0.5) / (H * 0.5))
                glow_t = glow_t * glow_t
                r = int(5 + glow_t * 80)
                g = int(5 + glow_t * 30)
                b = int(8 + glow_t * 10)
                img.putpixel((x, y), (r, g, b, 255))

    # --- 6. Stairs descending into darkness ---
    stair_left = int(W * 0.30)
    stair_right = int(W * 0.70)
    stair_count = 7
    stair_start = int(H * 0.55)
    stair_h = int(H * 0.05)

    for s in range(stair_count):
        sy = stair_start + s * stair_h
        # Each step narrows slightly (perspective)
        indent = s * int(W * 0.015)
        sl = stair_left + indent
        sr = stair_right - indent
        # Glow increases with depth
        glow = s / stair_count
        for y in range(sy, sy + stair_h - 1):
            for x in range(sl, sr):
                if 0 <= y < H:
                    noise = random.randint(-5, 5)
                    base_r = int(35 + glow * 60 + noise)
                    base_g = int(35 + glow * 20 + noise)
                    base_b = int(38 + noise)
                    # Step edge highlight
                    if y == sy:
                        base_r += 20; base_g += 15; base_b += 10
                    img.putpixel((x, y), (
                        max(0, min(255, base_r)),
                        max(0, min(255, base_g)),
                        max(0, min(255, base_b)), 255))

    # --- 7. Torches on pillars ---
    torch_positions = [
        (int(W * 0.18) + pillar_w + 8, int(H * 0.25)),
        (int(W * 0.82) - pillar_w - 12, int(H * 0.25)),
    ]
    for tx, ty in torch_positions:
        # Handle
        for dy in range(0, int(H * 0.08)):
            for dx in range(-1, 2):
                if 0 <= tx + dx < W and 0 <= ty + dy < H:
                    img.putpixel((tx + dx, ty + dy), (90, 65, 35, 255))
        # Flame (layered circles)
        flame_colors = [
            (255, 220, 80, 255),   # bright core
            (255, 150, 30, 255),   # mid
            (255, 80, 20, 255),    # outer
            (200, 40, 10, 255),    # tip
        ]
        for fr, fc in [(3, flame_colors[0]), (5, flame_colors[1]),
                       (7, flame_colors[2]), (4, flame_colors[3])]:
            fy_off = -fr - 2
            for dy in range(-fr, fr + 1):
                for dx in range(-fr, fr + 1):
                    if dx * dx + dy * dy <= fr * fr:
                        px_x, px_y = tx + dx, ty + fy_off + dy
                        if 0 <= px_x < W and 0 <= px_y < H:
                            img.putpixel((px_x, px_y), fc)
        # Ember particles
        for _ in range(6):
            ex = tx + random.randint(-10, 10)
            ey = ty - random.randint(12, 30)
            if 0 <= ex < W and 0 <= ey < H:
                img.putpixel((ex, ey), (255, 200, 50, 255))

    # --- 8. Adventurer silhouette at top of stairs (skipped for the animated walk clip) ---
    if with_figure:
        fig_x = W // 2
        fig_y = stair_start - 2
        fig_col = (15, 12, 10, 255)
        # Body
        for dy in range(-20, 0):
            w = 3 if dy < -14 else (5 if dy < -8 else 4)
            for dx in range(-w, w + 1):
                if 0 <= fig_x + dx < W and 0 <= fig_y + dy < H:
                    img.putpixel((fig_x + dx, fig_y + dy), fig_col)
        # Head (circle)
        for dx in range(-3, 4):
            for dy in range(-4, 1):
                if dx * dx + dy * dy <= 12:
                    px_x, px_y = fig_x + dx, fig_y - 22 + dy
                    if 0 <= px_x < W and 0 <= px_y < H:
                        img.putpixel((px_x, px_y), fig_col)
        # Sword (right side)
        for dy in range(-18, -5):
            sx = fig_x + 7
            if 0 <= sx < W and 0 <= fig_y + dy < H:
                img.putpixel((sx, fig_y + dy), (80, 80, 90, 255))
                img.putpixel((sx + 1, fig_y + dy), (60, 60, 70, 255))

    # (Title text and vignette moved to draw_title() / apply_vignette() so draw_scene_art stays
    #  text-free and reusable. draw_scene() re-composes all three for the original logo.)


def draw_title(img):
    """Overlay the stacked title wordmark: CURSE OF THE / DUNGEON / ENGINE (gold + red, shadowed)."""
    draw = ImageDraw.Draw(img)
    W, H = img.size

    try:
        font_title = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            int(H * 0.12))
        font_sub = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            int(H * 0.055))
    except (OSError, IOError):
        font_title = ImageFont.load_default()
        font_sub = ImageFont.load_default()

    # "CURSE OF THE" — smaller, above arch
    sub_text = "CURSE OF THE"
    sub_bbox = draw.textbbox((0, 0), sub_text, font=font_sub)
    sub_w = sub_bbox[2] - sub_bbox[0]
    draw.text(((W - sub_w) // 2, int(H * 0.02)), sub_text,
              fill=(200, 180, 120, 255), font=font_sub)

    # "DUNGEON" — large gold
    title_text = "DUNGEON"
    title_bbox = draw.textbbox((0, 0), title_text, font=font_title)
    title_w = title_bbox[2] - title_bbox[0]
    title_y = int(H * 0.07)
    draw.text(((W - title_w) // 2 + 2, title_y + 2), title_text,
              fill=(20, 15, 10, 200), font=font_title)
    draw.text(((W - title_w) // 2, title_y), title_text,
              fill=(255, 210, 60, 255), font=font_title)

    # "ENGINE" — even larger, red, below DUNGEON
    try:
        font_engine = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            int(H * 0.15))
    except (OSError, IOError):
        font_engine = font_title
    engine_text = "ENGINE"
    engine_bbox = draw.textbbox((0, 0), engine_text, font=font_engine)
    engine_w = engine_bbox[2] - engine_bbox[0]
    engine_y = title_y + int(H * 0.12) + 4
    draw.text(((W - engine_w) // 2 + 2, engine_y + 2), engine_text,
              fill=(40, 5, 5, 200), font=font_engine)
    draw.text(((W - engine_w) // 2, engine_y), engine_text,
              fill=(200, 40, 30, 255), font=font_engine)


def apply_vignette(img):
    """Darken the scene's edges with a soft radial falloff."""
    W, H = img.size
    for y in range(H):
        for x in range(W):
            dx = (x - W / 2) / (W / 2)
            dy = (y - H / 2) / (H / 2)
            vignette = max(0, 1.0 - (dx * dx + dy * dy) * 0.5)
            if vignette < 1.0:
                px = img.getpixel((x, y))
                img.putpixel((x, y), (
                    int(px[0] * vignette),
                    int(px[1] * vignette),
                    int(px[2] * vignette), px[3]))


def draw_scene(img):
    """Full logo art = dungeon scene + title wordmark + vignette (original behavior/output)."""
    draw_scene_art(img)
    draw_title(img)
    apply_vignette(img)


def main():
    print("Generating logo at 800x400...")
    img = Image.new("RGBA", (BW, BH), (0, 0, 0, 255))
    draw_scene(img)

    # Save all 3 sizes
    outputs = [
        ("logo_lg.png", 800, 400),
        ("logo_md.png", 512, 256),
        ("logo_sm.png", 256, 128),
    ]
    for name, w, h in outputs:
        path = os.path.join(ROOT_DIR, "assets", name)
        resized = img.resize((w, h), Image.LANCZOS) if (w, h) != (BW, BH) else img
        resized.save(path)
        print(f"  {path} ({w}x{h})")

    # Widescreen banner — rendered natively at 960x280 (not cropped)
    wide_w, wide_h = 960, 280
    wide_img = Image.new("RGBA", (wide_w, wide_h), (0, 0, 0, 255))
    draw_scene(wide_img)
    wide_path = os.path.join(ROOT_DIR, "assets", "logo_wide.png")
    wide_img.save(wide_path)
    print(f"  {wide_path} ({wide_w}x{wide_h})")

    # Also save as logo.png (512x256) for backward compat
    img.resize((512, 256), Image.LANCZOS).save(
        os.path.join(ROOT_DIR, "assets", "logo.png"))
    print("Done.")


if __name__ == "__main__":
    main()
