#!/usr/bin/env python3
"""Animated logo clip — the adventurer bops from the left to the center of the stairs landing,
then leans forward to look down the descending stairs.

Zoomed in on the landing/stairs so the little silhouette reads (no title text). Reuses the logo's
own dungeon scene art (gen_logo.draw_scene_art, rendered text-free AND figure-free) for a static
background, then composites an animated dark-silhouette walker on top each frame. Encodes an MP4
(H.264, for the editor) and a looping GIF (for quick preview).

Usage:
    python3 tools/gen_logo_walk.py [--outdir store/trailer] [--fps 24]
"""

import argparse
import math
import os
import subprocess
import sys
import tempfile

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

import gen_logo
from gen_trailer_frames import camera_crop, _vignette_fast   # DRY: reuse scene crop + fast vignette

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

W, H = 1920, 1080            # 1080p
STEEL = (150, 152, 165)      # rogue's dagger blade

# --- figure geometry, as fractions of the frame ---------------------------------------------
GROUND_Y = 0.52 * H          # where the feet rest (on the landing)
FIG_H    = 0.34 * H          # rogue height (15 voxels tall)
X_LEFT   = 0.36 * W          # walk start
X_CENTER = 0.50 * W          # walk end / look-down spot

# ----------------------------------------------------------------------------------------------
# Hooded voxel ROGUE — the actual playable class, recreated as a faithful 2D front sprite.
# Silhouette = the voxel boxes of gen_player_rogue (tools/gen_mesh.py:3621) projected to the
# (gx,gy) plane; colours = skin_player_rogue (tools/gen_skin.py:4047). Grid is 7 wide (px=gx+3)
# x 15 tall (gy 0=feet .. 14=hood crown). One colour per (gx,gy) column, exactly as the game's
# voxel-UV skin paints it.
# ----------------------------------------------------------------------------------------------
ROGUE_W, ROGUE_H = 7, 15

# Filled px columns per row gy (front projection of gen_player_rogue's fill_box calls).
ROW_PX = {
    0:  [2, 4],                 # boots
    1:  [2, 4],                 # shins
    2:  [2, 4],                 # thighs
    3:  [1, 2, 4, 5],           # thighs (2,4) + fingerless gloves at the wrists (1,5)
    4:  [1, 2, 3, 4, 5],        # pelvis/belt (2,3,4) + forearms (1,5)
    5:  [1, 2, 3, 4, 5],        # pelvis + forearms
    6:  [1, 2, 3, 4, 5],        # lower torso + forearms
    7:  [1, 2, 3, 4, 5],        # bandolier row + upper arms
    8:  [1, 2, 3, 4, 5],        # bandolier row + upper arms
    9:  [1, 2, 3, 4, 5],        # upper torso + shoulder caps
    10: [3],                    # neck
    11: [1, 2, 3, 4, 5],        # head — chin (face wrap)
    12: [1, 2, 3, 4, 5],        # head — cheeks (face wrap)
    13: [0, 1, 2, 3, 4, 5],     # eye row + hood side-drapes (eyes at px 2 & 4)
    14: [0, 1, 2, 3, 4, 5, 6],  # hood crown + forward brim (full width)
}

# Per-row palette (RGB), straight from skin_player_rogue.
_LEATHER    = (35, 35, 45)
_LEATHER_HI = (55, 55, 70)
_WRAP       = (75, 75, 85)
_STRAP      = (120, 80, 55)
_EYE        = (170, 200, 220)
_HOOD_SHA   = (20, 20, 28)
_BOOT       = (30, 30, 40)
_GLOVE      = (45, 45, 55)
ROW_COLOR = {
    0: _BOOT, 1: _BOOT, 2: _LEATHER, 3: _LEATHER, 4: _STRAP, 5: _LEATHER, 6: _LEATHER,
    7: _STRAP, 8: _STRAP, 9: _LEATHER_HI, 10: _WRAP, 11: _WRAP, 12: _WRAP,
    13: _HOOD_SHA, 14: _LEATHER_HI,
}


def cell_color(gy, px, look):
    """Colour of voxel column (px, gy). Eyes glow ice-blue (gy=13, px 2/4) while walking; when
    looking down (`look`>0.5) the brim shadows them out. Gloves brighten the wrist row."""
    if gy == 13 and px in (2, 4):
        return _HOOD_SHA if look > 0.5 else _EYE
    if gy == 3 and px in (1, 5):
        return _GLOVE
    return ROW_COLOR[gy]


def _shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c)


def draw_voxel(d, x, y, s, color):
    """Draw one voxel as a beveled cube: lighter top edge + darker bottom/right edge, so the
    flat sprite reads as 3D low-poly voxels (Barony style) rather than flat pixels."""
    x, y, s = int(round(x)), int(round(y)), int(round(s))
    if s < 1:
        return
    b = max(1, s // 5)
    d.rectangle([x, y, x + s - 1, y + s - 1], fill=_shade(color, 1.0))
    d.rectangle([x, y, x + s - 1, y + b - 1], fill=_shade(color, 1.30))          # top highlight
    d.rectangle([x, y + s - b, x + s - 1, y + s - 1], fill=_shade(color, 0.62))  # bottom shadow
    d.rectangle([x + s - b, y, x + s - 1, y + s - 1], fill=_shade(color, 0.78))  # right shadow


def draw_rogue(frame, cx, walk_phase, look):
    """Composite the hooded voxel rogue onto `frame` (RGBA), front-facing (oriented like the logo).
    cx = horizontal centre (px); walk_phase cycles the legs/arms/bob; look (0..1) dips the hood and
    kills the eye-glint to gaze down the stairs."""
    look = max(0.0, min(1.0, look))
    walking = 1.0 - look
    s = FIG_H / ROGUE_H                                    # voxel size (px)
    swing = math.sin(2 * math.pi * walk_phase) * walking   # +1 = right foot up / left foot up alt
    bob = -abs(math.sin(2 * math.pi * walk_phase)) * 0.5 * s * walking   # body bounce
    lean = look * 1.0 * s                                  # forward lean when looking down
    head_dip = look * 0.6 * s                              # hood tips down when looking down

    layer = Image.new("RGBA", frame.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)

    # origin: bottom-centre voxel (px=3, gy=0) sits at the feet. Grid is 7 wide, centred on cx.
    grid_left = cx - 3.5 * s
    feet_y = GROUND_Y

    # ground shadow under the feet
    sr = 3.0 * s
    d.ellipse([cx - sr, feet_y - 0.4 * s, cx + sr, feet_y + 0.7 * s], fill=(0, 0, 0, 95))

    # dagger in the right hand (px≈5, wrist row gy≈3): up while walking, lowered when looking down
    hand_x = grid_left + 5.4 * s
    hand_y = feet_y - (3.2 * s) + bob
    theta = math.radians(-72 + look * 150)
    dl = 3.2 * s
    tip = (hand_x + math.cos(theta) * dl, hand_y + math.sin(theta) * dl)
    d.line([(hand_x, hand_y), tip], fill=STEEL, width=max(2, int(0.45 * s)))
    d.line([(hand_x - 0.5 * s, hand_y), (hand_x + 0.7 * s, hand_y)],
           fill=_STRAP, width=max(2, int(0.32 * s)))   # crossguard / grip

    # voxel body, row by row (gy 0 at feet → 14 at hood crown; image y flips)
    for gy, cols in ROW_PX.items():
        # per-row vertical offset: legs alternate a small foot-lift; head/hood dips on look-down
        for px in cols:
            voff = bob
            xoff = 0.0
            if gy <= 1:                                   # feet/shins lift with the stride
                leg = -1 if px == 2 else 1                # px2 = left leg, px4 = right leg
                lift = max(0.0, swing * leg) * 1.1 * s    # that foot rises on its step
                voff -= lift
            if gy >= 4:                                   # whole upper body leans on look-down
                xoff += lean * ((gy - 4) / 10.0)
            if gy >= 10:                                  # head + hood drop when looking down
                voff += head_dip
            x = grid_left + px * s + xoff
            y = feet_y - (gy + 1) * s + voff
            draw_voxel(d, x, y, s + 1, cell_color(gy, px, look))

    frame.alpha_composite(layer)


def smoothstep(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


def build_background():
    """Static 1920x1080 background: the logo's dungeon scene (figure-free), pushed in on the stairs."""
    gen_logo.random.seed(42)
    master = Image.new("RGBA", (2560, 1440), (0, 0, 0, 255))
    gen_logo.draw_scene_art(master, with_figure=False)        # <-- the new flag
    master = _vignette_fast(master)
    return camera_crop(master, frac=0.60, cx=0.5, cy=0.66)    # frames the landing + descending stairs


def build_frames(fps):
    bg = build_background()

    # Timeline (seconds) -> frame counts
    seg = [("hold",  0.30), ("walk", 2.00), ("plant", 0.35), ("look", 0.35), ("end", 0.55)]
    counts = [(name, max(1, int(round(dur * fps)))) for name, dur in seg]

    frames = []
    walk_phase = 0.0
    for name, n in counts:
        for i in range(n):
            u = i / max(1, n - 1)
            if name == "hold":
                cx, look = X_LEFT, 0.0
            elif name == "walk":
                cx = X_LEFT + (X_CENTER - X_LEFT) * smoothstep(u)
                look = 0.0
                walk_phase = (walk_phase + 1.0 / 13.0) % 1.0       # ~step cadence
            elif name == "plant":
                cx, look = X_CENTER, 0.0
                walk_phase = (walk_phase + (1.0 / 13.0) * (1 - u)) % 1.0   # decelerate the stride
            elif name == "look":
                cx, look = X_CENTER, smoothstep(u)
                walk_phase = 0.0                                   # feet together (no swing)
            else:  # end — hold the look-down
                cx, look = X_CENTER, 1.0

            fr = bg.copy()
            draw_rogue(fr, cx, walk_phase, look)
            frames.append(fr.convert("RGB"))
    return frames


def encode_mp4(frames, path, fps):
    with tempfile.TemporaryDirectory() as td:
        for i, fr in enumerate(frames):
            fr.save(os.path.join(td, f"f_{i:04d}.png"))
        subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(fps),
             "-i", os.path.join(td, "f_%04d.png"),
             "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", path],
            check=True)


def encode_gif(frames, path, fps):
    # Downscale + decimate 2:1 to keep the GIF small; preview-grade.
    small = [f.resize((960, 540), Image.LANCZOS).convert("P", palette=Image.ADAPTIVE)
             for f in frames[::2]]
    small[0].save(path, save_all=True, append_images=small[1:],
                  duration=int(1000 / (fps / 2)), loop=0, optimize=True, disposal=2)


def main():
    ap = argparse.ArgumentParser(description="Animated logo walk clip.")
    ap.add_argument("--outdir", default=os.path.join(ROOT_DIR, "store", "trailer"))
    ap.add_argument("--fps", type=int, default=24)
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    print("Rendering frames...")
    frames = build_frames(args.fps)
    print(f"  {len(frames)} frames")

    mp4 = os.path.join(args.outdir, "logo_walk.mp4")
    gif = os.path.join(args.outdir, "logo_walk.gif")
    encode_mp4(frames, mp4, args.fps)
    encode_gif(frames, gif, args.fps)
    print(f"  {mp4}")
    print(f"  {gif}")
    print("Done.")


if __name__ == "__main__":
    main()
