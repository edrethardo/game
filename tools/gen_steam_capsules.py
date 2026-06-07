#!/usr/bin/env python3
"""Generate the Steam store capsule images for "Curse of the Dungeon Engine".

Steam requires a fixed set of branded capsules at exact pixel sizes. This tool composites
a key-art BACKGROUND (a real in-engine screenshot when supplied, otherwise a procedural
dungeon-ambiance fallback) with the game WORDMARK ("CURSE OF THE DUNGEON ENGINE") drawn
directly in the established gold/red styling — the shipped logo PNGs are baked on solid black
and cannot composite over art, so the wordmark is re-drawn here per gen_logo.py's look.

Outputs (into store/steam/, English default slot — no language suffix needed):
    header_capsule.png    920 x 430   wordmark prominent over key art
    small_capsule.png     462 x 174   wordmark nearly fills (must read at tiny size)
    main_capsule.png     1232 x 706   full key art, wordmark lower third
    vertical_capsule.png  748 x 896   portrait key-art crop + wordmark near top
    page_background.png  1438 x 810   ambient, low-contrast, NO wordmark (Steam tints/fades)

Hero shots are captured in-engine with the F8 debug key (F10 hides the HUD, F2 noclip frames),
which writes screenshot_*.png to the game's working directory. Point this tool at them:

    python3 tools/gen_steam_capsules.py --landscape screenshot_00042.png \
                                        --portrait  screenshot_00088.png

With no shots given it falls back to the procedural background and still emits all five, so the
set is never broken. Deterministic (fixed seed) so reruns are stable.

Usage:
    python3 tools/gen_steam_capsules.py [--landscape PNG] [--portrait PNG] [--outdir DIR]
"""

import argparse
import glob
import math
import os
import random
import sys

try:
    from PIL import Image, ImageDraw, ImageFont, ImageChops, ImageFilter
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)

random.seed(42)  # deterministic procedural fallback

FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

# Brand palette (matches gen_logo.py / in-game title colors)
GOLD_SUB = (200, 180, 120)   # "CURSE OF THE"
GOLD     = (255, 210, 60)    # "DUNGEON"
RED      = (200, 40, 30)     # "ENGINE"

# Steam capsule target sizes
HEADER     = (920, 430)
SMALL      = (462, 174)
MAIN       = (1232, 706)
VERTICAL   = (748, 896)
BACKGROUND = (1438, 810)


# ---------------------------------------------------------------------------
# Image helpers
# ---------------------------------------------------------------------------
def _font(px):
    try:
        return ImageFont.truetype(FONT_PATH, int(px))
    except (OSError, IOError):
        return ImageFont.load_default()


def fit_cover(src, w, h, focus_x=0.5, focus_y=0.5):
    """Scale `src` to COVER (w x h) without distortion, then crop with a focal bias."""
    src = src.convert("RGBA")
    sw, sh = src.size
    scale = max(w / sw, h / sh)
    nw, nh = max(w, int(round(sw * scale))), max(h, int(round(sh * scale)))
    resized = src.resize((nw, nh), Image.LANCZOS)
    # Crop window, biased toward the focal point but clamped inside the image.
    left = int(round((nw - w) * focus_x))
    top = int(round((nh - h) * focus_y))
    left = max(0, min(left, nw - w))
    top = max(0, min(top, nh - h))
    return resized.crop((left, top, left + w, top + h))


def _radial_mask(w, h, cx, cy, inner, outer, lo=0, hi=255):
    """Low-res radial gradient (lo..hi by distance), upscaled — cheap soft falloff."""
    bw, bh = 96, max(1, int(96 * h / w))
    m = Image.new("L", (bw, bh), 0)
    px = m.load()
    bcx, bcy = cx * bw / w, cy * bh / h
    bin_r, bout_r = inner * bw / w, outer * bw / w
    span = max(1.0, (bout_r - bin_r))
    for y in range(bh):
        for x in range(bw):
            d = math.hypot(x - bcx, y - bcy)
            t = (d - bin_r) / span
            t = 0.0 if t < 0 else (1.0 if t > 1 else t)
            px[x, y] = int(lo + (hi - lo) * t)
    return m.resize((w, h), Image.BILINEAR)


def draw_ambient_scene(size):
    """Procedural dungeon-entrance ambiance (NO text) — the key-art fallback when no
    screenshot is supplied. Uses ImageDraw/ImageChops only (no per-pixel Python loops over
    the full image) so it is fast at any capsule size. Mirrors gen_logo.py's mood: dark
    blue-black gradient, warm hell-glow from below, stone pillars + arch, torch flames."""
    w, h = size
    img = Image.new("RGBA", size, (0, 0, 0, 255))
    draw = ImageDraw.Draw(img)

    # 1. Vertical gradient (deep blue-black at top → slightly warmer/lighter at bottom).
    for y in range(h):
        t = y / h
        draw.line([(0, y), (w, y)],
                  fill=(int(8 + t * 14), int(8 + t * 9), int(15 + t * 6), 255))

    # 2. Warm "hell glow" rising from the bottom-center (additive, soft radial).
    glow = _radial_mask(w, h, w * 0.5, h * 1.02, h * 0.0, h * 0.85, lo=200, hi=0)
    glow_rgb = Image.merge("RGB", (
        glow.point(lambda v: int(v * 0.85)),   # R
        glow.point(lambda v: int(v * 0.35)),   # G
        glow.point(lambda v: int(v * 0.08)),   # B
    ))
    img = Image.alpha_composite(img, glow_rgb.convert("RGBA"))
    draw = ImageDraw.Draw(img)

    # 3. Stone pillars (flat gray slabs with darker edges) + connecting arch.
    pillar_w = int(w * 0.11)
    pillar_top = int(h * 0.06)
    pillar_bot = int(h * 0.92)
    for px in (int(w * 0.16), int(w * 0.84) - pillar_w):
        draw.rectangle([px, pillar_top, px + pillar_w, pillar_bot], fill=(58, 58, 62, 255))
        draw.rectangle([px, pillar_top, px + 4, pillar_bot], fill=(34, 34, 38, 255))            # left edge
        draw.rectangle([px + pillar_w - 4, pillar_top, px + pillar_w, pillar_bot], fill=(34, 34, 38, 255))
        draw.rectangle([px - 5, pillar_top - int(h * 0.04), px + pillar_w + 5, pillar_top],
                       fill=(72, 72, 76, 255))     # cap block

    arch_box = [int(w * 0.16), pillar_top - int(h * 0.16),
                int(w * 0.84), pillar_top + int(h * 0.22)]
    draw.arc(arch_box, start=180, end=360, fill=(70, 68, 66, 255), width=max(6, int(h * 0.05)))

    # 4. Torch flames on the inner pillar faces (layered warm ellipses).
    for tx in (int(w * 0.16) + pillar_w + int(w * 0.02),
               int(w * 0.84) - pillar_w - int(w * 0.02)):
        ty = int(h * 0.30)
        for rad, col in ((int(h * 0.05), (255, 80, 20, 255)),
                         (int(h * 0.035), (255, 150, 30, 255)),
                         (int(h * 0.02), (255, 220, 90, 255))):
            draw.ellipse([tx - rad, ty - rad * 2, tx + rad, ty + rad], fill=col)

    # 5. Vignette: darken edges by multiplying with a radial brightness mask.
    vign = _radial_mask(w, h, w * 0.5, h * 0.5, min(w, h) * 0.25, max(w, h) * 0.72, lo=255, hi=70)
    img = ImageChops.multiply(img, Image.merge("RGBA", (vign, vign, vign,
                                                        Image.new("L", size, 255))))
    return img


def darken_region(img, top_frac, bottom_frac, strength=160):
    """Overlay a soft dark band (for wordmark legibility / mood) between two height fractions,
    feathered at the inner edge. strength = peak darkness alpha (0..255)."""
    w, h = img.size
    y0, y1 = int(h * top_frac), int(h * bottom_frac)
    band = Image.new("L", (1, h), 0)
    bp = band.load()
    for y in range(h):
        if y0 <= y <= y1:
            # Feather: full strength toward the outer edge, fading across the band.
            edge = (y - y0) / max(1, (y1 - y0))
            bp[0, y] = int(strength * (edge if top_frac < 0.5 else (1.0 - edge)))
    mask = band.resize((w, h))
    overlay = Image.new("RGBA", (w, h), (3, 4, 8, 0))
    overlay.putalpha(mask)
    return Image.alpha_composite(img.convert("RGBA"), overlay)


def make_wordmark(target_w, max_h=None):
    """Render the stacked wordmark on a transparent tile, scaled to `target_w` wide — but
    never taller than `max_h` (scaled down uniformly if the width would overflow the box).

    Lines (matching gen_logo.py): "CURSE OF THE" (small gold), "DUNGEON" (large gold),
    "ENGINE" (largest red), each with a dark drop shadow. Returns a tight RGBA image."""
    # Render large for crisp downscaling, then fit to target width.
    s_sub, s_dun, s_eng = 64, 150, 188
    f_sub, f_dun, f_eng = _font(s_sub), _font(s_dun), _font(s_eng)
    pad = 36
    scratch = ImageDraw.Draw(Image.new("RGBA", (1, 1)))

    def line_size(text, font):
        b = scratch.textbbox((0, 0), text, font=font)
        return b[2] - b[0], b[3] - b[1]

    sub_w, sub_h = line_size("CURSE OF THE", f_sub)
    dun_w, dun_h = line_size("DUNGEON", f_dun)
    eng_w, eng_h = line_size("ENGINE", f_eng)

    gap = int(s_dun * 0.06)
    canvas_w = max(sub_w, dun_w, eng_w) + pad * 2
    canvas_h = sub_h + dun_h + eng_h + gap * 2 + pad * 2
    tile = Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))
    d = ImageDraw.Draw(tile)

    def centered(text, font, y, fill, shadow):
        tw = scratch.textbbox((0, 0), text, font=font)[2]
        x = (canvas_w - tw) // 2
        off = max(2, font.size // 40)
        d.text((x + off, y + off), text, font=font, fill=shadow)
        d.text((x, y), text, font=font, fill=fill)

    y = pad
    centered("CURSE OF THE", f_sub, y, GOLD_SUB + (255,), (30, 22, 8, 220)); y += sub_h + gap
    centered("DUNGEON", f_dun, y, GOLD + (255,), (20, 15, 5, 220));          y += dun_h + gap
    centered("ENGINE", f_eng, y, RED + (255,), (40, 5, 5, 220))

    tile = tile.crop(tile.getbbox())   # tighten to actual ink
    tw, th = tile.size
    scale = target_w / tw
    if max_h is not None and th * scale > max_h:
        scale = max_h / th             # height-limited: keep the mark inside the box
    return tile.resize((max(1, int(tw * scale)), max(1, int(th * scale))), Image.LANCZOS)


def paste_wordmark(canvas, target_w, cx_frac=0.5, cy_frac=0.5, anchor="center", max_h=None):
    """Scale the wordmark to `target_w` (clamped to `max_h` tall) and paste it onto `canvas`
    (alpha-composited). anchor: 'center' aligns by middle, 'top' aligns the mark's top to cy_frac."""
    mark = make_wordmark(target_w, max_h=max_h)
    w, h = canvas.size
    mw, mh = mark.size
    x = int(w * cx_frac - mw / 2)
    y = int(h * cy_frac) if anchor == "top" else int(h * cy_frac - mh / 2)
    layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    layer.paste(mark, (x, y), mark)
    return Image.alpha_composite(canvas, layer)


# ---------------------------------------------------------------------------
# Background resolution
# ---------------------------------------------------------------------------
def background(size, shot_path, focus_y=0.5):
    """Key-art background filling `size`: a real screenshot if available, else procedural."""
    if shot_path and os.path.isfile(shot_path):
        return fit_cover(Image.open(shot_path), size[0], size[1], focus_y=focus_y)
    return draw_ambient_scene(size)


def newest_screenshot():
    """Most recent screenshot_*.png from the repo root / CWD, if any (for default behavior)."""
    cands = glob.glob(os.path.join(ROOT_DIR, "screenshot_*.png")) + glob.glob("screenshot_*.png")
    return max(cands, key=os.path.getmtime) if cands else None


# ---------------------------------------------------------------------------
# Per-capsule composition
# ---------------------------------------------------------------------------
def build_header(land):
    img = background(HEADER, land)
    img = darken_region(img, 0.45, 1.0, strength=150)            # darken lower half for text
    img = paste_wordmark(img, int(HEADER[0] * 0.74), cy_frac=0.52, max_h=int(HEADER[1] * 0.86))
    return img


def build_small(land):
    img = background(SMALL, land)
    img = darken_region(img, 0.0, 1.0, strength=140)             # heavy overall darken
    # Logo nearly fills (Steam legibility rule), but clamp height so ENGINE isn't clipped.
    img = paste_wordmark(img, int(SMALL[0] * 0.92), cy_frac=0.5, max_h=int(SMALL[1] * 0.84))
    return img


def build_main(land):
    img = background(MAIN, land)
    img = darken_region(img, 0.5, 1.0, strength=150)
    img = paste_wordmark(img, int(MAIN[0] * 0.58), cy_frac=0.76,  # lower third, art breathes above
                         max_h=int(MAIN[1] * 0.42))
    return img


def build_vertical(port):
    img = background(VERTICAL, port, focus_y=0.4)
    img = darken_region(img, 0.0, 0.40, strength=170)            # darken top for the wordmark
    img = paste_wordmark(img, int(VERTICAL[0] * 0.82), cy_frac=0.06, anchor="top",
                         max_h=int(VERTICAL[1] * 0.34))
    return img


def build_background(land):
    img = background(BACKGROUND, land)
    # Ambient only — NO wordmark. Mild contrast reduction; Steam applies its own blue tint + fade.
    img = Image.blend(img, Image.new("RGBA", BACKGROUND, (20, 22, 30, 255)), 0.18)
    return img


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Generate Steam store capsules.")
    ap.add_argument("--landscape", help="landscape hero screenshot for wide capsules")
    ap.add_argument("--portrait", help="portrait/tall hero screenshot for the vertical capsule")
    ap.add_argument("--outdir", default=os.path.join(ROOT_DIR, "store", "steam"))
    args = ap.parse_args()

    land = args.landscape or newest_screenshot()
    port = args.portrait or land   # fall back to the landscape shot (cropped) if no portrait given

    os.makedirs(args.outdir, exist_ok=True)
    print(f"Background source — landscape: {land or '(procedural)'}, portrait: {port or '(procedural)'}")

    outputs = [
        ("header_capsule.png",   build_header(land)),
        ("small_capsule.png",    build_small(land)),
        ("main_capsule.png",     build_main(land)),
        ("vertical_capsule.png", build_vertical(port)),
        ("page_background.png",  build_background(land)),
    ]
    for name, img in outputs:
        path = os.path.join(args.outdir, name)
        img.convert("RGB").save(path)   # capsules are opaque; RGB keeps files lean
        print(f"  {path} ({img.size[0]}x{img.size[1]})")
    print("Done.")


if __name__ == "__main__":
    main()
