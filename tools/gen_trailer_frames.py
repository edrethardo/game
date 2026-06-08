#!/usr/bin/env python3
"""Generate trailer title frames — a word-by-word zoom-out of the logo.

Five 1080p stills that, played in sequence, read as a camera pushing out of the logo while the
title assembles word by word:

    CURSE  ->  OF  ->  THE  ->  DUNGEON + ENGINE  ->  (full logo)

The shipped logo PNGs are tiny (<=960 px), so cropping a single word up to 1080p would be blurry.
Instead we re-render the logo's dungeon scene ONCE at high resolution (text-free) and, per frame,
crop a progressively wider 16:9 "camera" window (the zoom-out), scale it to 1920x1080, and draw the
crisp word(s) centered on top in the logo's gold/red style. Frame 5 is the genuine full logo
(gen_logo.draw_title), so the sequence pays off on the real thing.

Reuses tools/gen_logo.py (draw_scene_art / draw_title) — the same art as assets/logo_wide.png.

Usage:
    python3 tools/gen_trailer_frames.py [--master 3840] [--outdir store/trailer]
"""

import argparse
import math
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont, ImageChops, ImageFilter
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

# gen_logo lives in this same tools/ dir (on sys.path when run as a script), like gen_steam_capsules.
import gen_logo

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

FRAME_W, FRAME_H = 1920, 1080      # 1080p output

# Brand palette (matches gen_logo.draw_title)
GOLD_SUB = (200, 180, 120)         # "CURSE OF THE" subtitle words
GOLD     = (255, 210, 60)          # "DUNGEON"
RED      = (200, 40, 30)           # "ENGINE"


def _font(px):
    try:
        return ImageFont.truetype(FONT_PATH, max(1, int(px)))
    except (OSError, IOError):
        return ImageFont.load_default()


def _vignette_fast(img):
    """Soft radial edge-darkening via a low-res L mask (cheap at 4K — avoids gen_logo.apply_vignette's
    per-pixel loop over millions of pixels)."""
    w, h = img.size
    bw, bh = 128, max(1, int(128 * h / w))
    m = Image.new("L", (bw, bh), 0)
    px = m.load()
    cx, cy = bw / 2.0, bh / 2.0
    for y in range(bh):
        for x in range(bw):
            dx, dy = (x - cx) / cx, (y - cy) / cy
            v = max(0.0, 1.0 - (dx * dx + dy * dy) * 0.5)     # same falloff as gen_logo.apply_vignette
            px[x, y] = int(255 * (0.28 + 0.72 * v))           # keep a floor so corners aren't crushed
    mask = m.resize((w, h), Image.BILINEAR)
    return ImageChops.multiply(img, Image.merge("RGBA", (mask, mask, mask,
                                                         Image.new("L", (w, h), 255))))


def render_scene_master(side_w):
    """Render the text-free dungeon scene at 16:9 (side_w wide) once, to crop every frame from."""
    w = side_w
    h = int(round(side_w * 9 / 16))
    gen_logo.random.seed(42)                # deterministic noise
    img = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    gen_logo.draw_scene_art(img)            # pillars / arch / doorway / stairs / torches / glow
    return _vignette_fast(img)


def camera_crop(master, frac, cx=0.5, cy=0.44):
    """Crop a centered 16:9 window covering `frac` of the master (frac<1 = zoomed in), focused at
    (cx, cy), then scale to 1920x1080. cy=0.44 frames arch -> doorway -> glowing stairs so the
    dungeon scene is always visible (not just the black doorway). Soft when zoomed (depth-of-field)
    — text is drawn crisp on top afterwards."""
    mw, mh = master.size
    cw, ch = mw * frac, mh * frac
    left = cx * mw - cw / 2.0
    top = cy * mh - ch / 2.0
    left = max(0.0, min(left, mw - cw))     # clamp inside the master
    top = max(0.0, min(top, mh - ch))
    crop = master.crop((int(left), int(top), int(left + cw), int(top + ch)))
    return crop.resize((FRAME_W, FRAME_H), Image.LANCZOS)


def _make_line(text, font, fill, shadow):
    """Render one text line on a tight transparent tile, with a dark drop shadow (logo style)."""
    scratch = ImageDraw.Draw(Image.new("RGBA", (1, 1)))
    bb = scratch.textbbox((0, 0), text, font=font)
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    off = max(2, font.size // 40)
    pad = off + max(6, font.size // 12)
    tile = Image.new("RGBA", (tw + pad * 2, th + pad * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(tile)
    # bb[0]/bb[1] can be nonzero (glyph side bearing) — subtract so the ink lands at the padding origin.
    ox, oy = pad - bb[0], pad - bb[1]
    d.text((ox + off, oy + off), text, font=font, fill=shadow + (210,))
    d.text((ox, oy), text, font=font, fill=fill + (255,))
    return tile.crop(tile.getbbox())


def draw_word(frame, lines, width_frac=0.6, cy_frac=0.5, gap_frac=0.04):
    """Composite centered word(s) onto `frame`, scaled so the widest line is width_frac of the frame,
    with a soft dark halo behind for legibility over the scene. `lines` = [(text, fill_rgb), ...]."""
    base = 320                                              # render large, then scale to target width
    tiles = [_make_line(t, _font(base), c, (15, 10, 4)) for t, c in lines]
    gap = int(base * gap_frac)
    block_w = max(t.width for t in tiles)
    block_h = sum(t.height for t in tiles) + gap * (len(tiles) - 1)

    scale = (FRAME_W * width_frac) / block_w
    # Clamp so tall stacked blocks never overflow vertically.
    if block_h * scale > FRAME_H * 0.78:
        scale = (FRAME_H * 0.78) / block_h

    block = Image.new("RGBA", (block_w, block_h), (0, 0, 0, 0))
    y = 0
    for t in tiles:
        block.alpha_composite(t, ((block_w - t.width) // 2, y))
        y += t.height + gap
    block = block.resize((max(1, int(block_w * scale)), max(1, int(block_h * scale))), Image.LANCZOS)

    bw, bh = block.size
    x = (FRAME_W - bw) // 2
    yy = int(FRAME_H * cy_frac - bh / 2)

    # Soft dark halo (blurred black silhouette, composited twice) — same idea as the capsule wordmark.
    halo = Image.new("RGBA", frame.size, (0, 0, 0, 0))
    sil = Image.new("RGBA", block.size, (0, 0, 0, 0))
    sil.putalpha(block.split()[3])
    halo.paste(sil, (x, yy), sil)
    halo = halo.filter(ImageFilter.GaussianBlur(radius=max(4, bh // 14)))
    out = frame
    out = Image.alpha_composite(out, halo)
    out = Image.alpha_composite(out, halo)

    layer = Image.new("RGBA", frame.size, (0, 0, 0, 0))
    layer.paste(block, (x, yy), block)
    return Image.alpha_composite(out, layer)


# Frame definitions: (filename, camera-fraction, builder). Camera fracs grow → progressive zoom-out.
def build_frames(master):
    # Camera fractions grow 0.50 -> 1.0 = a steady pull-back; each still shows the dungeon (pillars,
    # arch, glowing stairs) with the word over the dark doorway band (halo keeps it legible).
    f1 = draw_word(camera_crop(master, 0.50), [("CURSE", GOLD_SUB)], width_frac=0.48, cy_frac=0.40)
    f2 = draw_word(camera_crop(master, 0.62), [("OF",    GOLD_SUB)], width_frac=0.22, cy_frac=0.40)
    f3 = draw_word(camera_crop(master, 0.75), [("THE",   GOLD_SUB)], width_frac=0.30, cy_frac=0.40)
    f4 = draw_word(camera_crop(master, 0.90), [("DUNGEON", GOLD), ("ENGINE", RED)],
                   width_frac=0.74, cy_frac=0.44, gap_frac=0.02)

    # Frame 5 — the genuine full logo at 1080p: full master + the real stacked title.
    f5 = master.resize((FRAME_W, FRAME_H), Image.LANCZOS).copy()
    gen_logo.draw_title(f5)

    return [
        ("frame_1_curse.png",          f1),
        ("frame_2_of.png",             f2),
        ("frame_3_the.png",            f3),
        ("frame_4_dungeon_engine.png", f4),
        ("frame_5_logo.png",           f5),
    ]


def main():
    ap = argparse.ArgumentParser(description="Generate trailer title frames (word-by-word logo zoom).")
    ap.add_argument("--master", type=int, default=3840,
                    help="Width of the high-res scene master to crop from (default 3840 = 4K).")
    ap.add_argument("--outdir", default=os.path.join(ROOT_DIR, "store", "trailer"))
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    print(f"Rendering {args.master}x{int(args.master*9/16)} scene master...")
    master = render_scene_master(args.master)

    for name, img in build_frames(master):
        path = os.path.join(args.outdir, name)
        img.convert("RGB").save(path)
        print(f"  {path} ({img.size[0]}x{img.size[1]})")
    print("Done.")


if __name__ == "__main__":
    main()
