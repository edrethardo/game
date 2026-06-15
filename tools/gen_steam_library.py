#!/usr/bin/env python3
"""Generate the Steam *Library* asset set for "Curse of the Dungeon Engine".

Distinct from the store capsules (gen_steam_capsules.py); these are the library-detail/collection
images. Reuses the exact same dungeon scene art + gold/red wordmark + dark-halo legibility so the
Library set matches the store set.

Outputs (into store/steam/, English default slot — no language suffix):
    library_capsule.png   600 x 900    portrait key art + legible logo
    library_header.png    920 x 430    branding (== the store Header Capsule)
    library_hero.png     3840 x 1240   rich scene, NO text/logo, art centered in the safe area
    library_logo.png     <=1280 x 720  the wordmark on a TRANSPARENT background (drop shadow)

Hero/capsule accept an optional real screenshot; otherwise they fall back to the logo's scene art.

Usage:
    python3 tools/gen_steam_library.py [--landscape PNG] [--portrait PNG] [--outdir DIR]
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageFilter
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

import gen_steam_capsules as cap                 # scene bg, wordmark, halo, palette, fit_cover...
from gen_trailer_frames import render_scene_master  # sharp full-res scene for the big hero

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LIB_CAPSULE = (600, 900)
LIB_HEADER  = (920, 430)
LIB_HERO    = (3840, 1240)
LOGO_MAX_W, LOGO_MAX_H = 1280, 720


def build_library_capsule(port):
    """600x900 portrait key art with the legible wordmark up top."""
    img = cap.background(LIB_CAPSULE, port, portrait=True)
    img = cap.darken_region(img, 0.0, 0.46, strength=120)        # seat the wordmark band
    img = cap.paste_wordmark(img, int(LIB_CAPSULE[0] * 0.84), cy_frac=0.06, anchor="top",
                             max_h=int(LIB_CAPSULE[1] * 0.30))
    return img.convert("RGB")


def build_library_header(land):
    """920x430 branding — identical treatment to the store Header Capsule (Steam falls back to it)."""
    return cap.build_header(land).convert("RGB")


def build_library_hero(land):
    """3840x1240 visually-rich scene, NO text/logo. Doorway/arch/stairs kept in the centered safe
    area. Uses the full-res scene renderer (capsule render_scene_bg caps at 1100px → too soft here)."""
    if land and os.path.isfile(land):
        hero = cap.fit_cover(Image.open(land), LIB_HERO[0], LIB_HERO[1], focus_y=0.42)
    else:
        # Render LARGER than the hero, then crop a centred 3840x1240 window zoomed onto the warm
        # glowing stairs + torch-lit pillars (the rich, evocative key art) — a thin full-width band
        # of the whole scene would be dominated by the black doorway. Sharp (crop, no upscale).
        master = render_scene_master(5120)                      # 5120x2880 sharp, text-free
        mw, mh = master.size
        left = (mw - LIB_HERO[0]) // 2                          # centred horizontally (pillars framed)
        top = int(mh * 0.70 - LIB_HERO[1] / 2)                  # centred on the descending stairs
        top = max(0, min(top, mh - LIB_HERO[1]))
        hero = master.crop((left, top, left + LIB_HERO[0], top + LIB_HERO[1]))
    return hero.convert("RGB")


def build_library_logo():
    """The wordmark on a TRANSPARENT background, fit within 1280x720, with a soft dark glow baked in
    (drop shadow per Steam's advice) so it stays legible over the hero. Saved RGBA."""
    mark = cap.make_wordmark(LOGO_MAX_W)                        # tight RGBA, width == 1280
    w, h = mark.size
    if h > LOGO_MAX_H:                                          # also fit within 720 tall
        s = LOGO_MAX_H / h
        mark = mark.resize((max(1, int(w * s)), LOGO_MAX_H), Image.LANCZOS)

    # Pad by the blur radius so the glow isn't clipped at the edges.
    rad = max(6, mark.size[1] // 16)
    pad = rad * 3
    canvas = Image.new("RGBA", (mark.size[0] + pad * 2, mark.size[1] + pad * 2), (0, 0, 0, 0))
    canvas.paste(mark, (pad, pad), mark)

    sil = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    sil.putalpha(canvas.split()[3])                            # black silhouette from the alpha
    glow = sil.filter(ImageFilter.GaussianBlur(rad))
    out = Image.alpha_composite(glow, glow)                    # denser core
    out = Image.alpha_composite(out, canvas)                   # wordmark on top
    out = out.crop(out.getbbox())                              # trim to ink+glow, stay transparent
    # The glow widened the tile past 1280 — clamp the FINAL image to <=1280x720 (Steam's limit).
    w, h = out.size
    if w > LOGO_MAX_W or h > LOGO_MAX_H:
        s = min(LOGO_MAX_W / w, LOGO_MAX_H / h)
        out = out.resize((max(1, int(w * s)), max(1, int(h * s))), Image.LANCZOS)
    return out


def main():
    ap = argparse.ArgumentParser(description="Generate Steam Library assets.")
    ap.add_argument("--landscape", help="landscape hero screenshot (header + hero)")
    ap.add_argument("--portrait", help="portrait screenshot (library capsule)")
    ap.add_argument("--outdir", default=os.path.join(ROOT_DIR, "store", "steam"))
    args = ap.parse_args()

    land = args.landscape or cap.newest_screenshot()
    port = args.portrait or land

    os.makedirs(args.outdir, exist_ok=True)
    print(f"Background source — landscape: {land or '(procedural)'}, portrait: {port or '(procedural)'}")

    outputs = [
        ("library_capsule.png", build_library_capsule(port)),
        ("library_header.png",  build_library_header(land)),
        ("library_hero.png",    build_library_hero(land)),
        ("library_logo.png",    build_library_logo()),         # RGBA (transparent)
    ]
    for name, img in outputs:
        path = os.path.join(args.outdir, name)
        img.save(path)
        print(f"  {path} ({img.size[0]}x{img.size[1]}, {img.mode})")
    print("Done.")


if __name__ == "__main__":
    main()
