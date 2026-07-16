#!/usr/bin/env python3
"""Generate Steam achievement icons (64x64 opaque PNGs, achieved + locked pair per achievement).

Art is authored as 16x16 ASCII pixel grids (the game's chunky pixel-art identity — same idiom
as tools/gen_status_icons.py: one char per pixel, '.' = background plate, letters = palette
entries) and upscaled 4x nearest-neighbour to Steam's required 64x64. Each achievement emits:

    store/steam/achievements/<API_NAME>.png          — achieved (full color)
    store/steam/achievements/<API_NAME>_locked.png   — unachieved (desaturated + dimmed;
                                                       Steam does NOT auto-gray icons)

Both are OPAQUE (dark plate background, no alpha) — the Steam client composites icons on dark
UI and semi-transparent PNGs render unpredictably.

Marketing/store asset like gen_steam_capsules.py: run on demand, NOT part of build_assets.py.
The PNGs under store/ ARE committed (unlike assets/meshes). Upload both files per achievement
on the Steamworks partner site and PUBLISH — see docs/DEPLOYMENT.md → Achievements.

Usage:
    python3 tools/gen_achievement_icons.py            # writes all icons
"""

import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow required: pip install Pillow")

SIZE  = 16   # authored grid
SCALE = 4    # 16 * 4 = Steam's mandatory 64x64
PLATE = (30, 32, 38)          # opaque background plate
LOCKED_DIM = 0.55             # locked = grayscale luminance * this

# ---------------------------------------------------------------------------
# Achievement art. Every grid must be exactly 16 rows x 16 chars; every non-'.'
# char must exist in the palette. Violations fail loudly (never ship a blank).
# ---------------------------------------------------------------------------

ACHIEVEMENTS = {
    # First item picked up from the world: a bulging coin pouch, gold tie, spilled
    # coins, and the four-point sparkle the loot beacons taught players to chase.
    "ACH_FIRST_ITEM": {
        "palette": {
            "b": (122, 82, 46),    # sack leather
            "l": (150, 104, 60),   # leather highlight
            "r": (212, 168, 66),   # tie rope (gold)
            "g": (240, 196, 60),   # coin gold
            "G": (180, 140, 40),   # coin shading
            "w": (255, 244, 200),  # sparkle
        },
        "art": [
            "..............w.",
            ".....rr......www",
            "....r..r......w.",
            "....rrrr........",
            "...bbllbb.......",
            "..bbllllbb......",
            ".bbllllllbb.....",
            ".blllllllbb.....",
            ".bbllllllbb.....",
            ".bbbllllbbb.....",
            ".bbbbbbbbbb.....",
            "..bbbbbbbb......",
            "...bbbbbb.......",
            ".........ggg....",
            "........ggGg.gg.",
            ".........gg..gG.",
        ],
    },
    # All seven equipment slots filled: closed helm over a gold-trimmed chestplate
    # with pauldrons — the silhouette of a hero who is finally wearing everything.
    "ACH_FULLY_EQUIPPED": {
        "palette": {
            "h": (168, 178, 196),  # steel light
            "H": (120, 130, 150),  # steel dark
            "e": (16, 17, 20),     # visor slit
            "t": (212, 168, 66),   # gold trim
            "p": (140, 150, 170),  # pauldron steel
        },
        "art": [
            "......hhhh......",
            ".....hhhhhh.....",
            ".....hhhhhh.....",
            ".....heeeeh.....",
            ".....hhhhhh.....",
            "......hhhh......",
            "..pphhhhhhhhpp..",
            ".ppphhhtthhhppp.",
            ".pp.hhhtthhh.pp.",
            "....hhhtthhh....",
            "....hhhtthhh....",
            ".....hhtthh.....",
            ".....hhtthh.....",
            "......htth......",
            "......HHHH......",
            "................",
        ],
    },
    # Killed by The Butcher (floor-5 boss): his cleaver, edge dripping.
    "ACH_BUTCHERED": {
        "palette": {
            "o": (122, 82, 46),    # handle wood
            "O": (90, 60, 34),     # handle shading
            "s": (190, 198, 212),  # blade steel
            "S": (140, 148, 164),  # blade shading
            "E": (230, 236, 246),  # cutting-edge highlight
            "x": (196, 40, 40),    # blood
            "X": (150, 24, 24),    # blood dark
        },
        "art": [
            "............oo..",
            "...........ooO..",
            "..........ooO...",
            ".........ooO....",
            ".sssssssssoO....",
            ".sssssssssss....",
            ".sssSssssssss...",
            ".ssssssssssss...",
            ".ssssssssssss...",
            ".sssssssssssS...",
            ".EEEEEEEEEEEE...",
            "....x...x.......",
            "....X...x.......",
            "........X.......",
            "................",
            "................",
        ],
    },
}

# ---------------------------------------------------------------------------


def validate(name, spec):
    art, pal = spec["art"], spec["palette"]
    if len(art) != SIZE:
        sys.exit(f"{name}: {len(art)} rows (want {SIZE})")
    for y, row in enumerate(art):
        if len(row) != SIZE:
            sys.exit(f"{name} row {y}: {len(row)} chars (want {SIZE})")
        for ch in row:
            if ch != "." and ch not in pal:
                sys.exit(f"{name} row {y}: unknown palette char '{ch}'")
    if all(ch == "." for row in art for ch in row):
        sys.exit(f"{name}: art is entirely empty")


def render(spec, locked):
    img = Image.new("RGB", (SIZE, SIZE), PLATE)
    px = img.load()
    for y, row in enumerate(spec["art"]):
        for x, ch in enumerate(row):
            if ch == ".":
                continue
            r, g, b = spec["palette"][ch]
            if locked:
                # Rec.601 luminance, dimmed — the standard "not earned yet" gray.
                lum = int((0.299 * r + 0.587 * g + 0.114 * b) * LOCKED_DIM)
                r = g = b = lum
            px[x, y] = (r, g, b)
    return img.resize((SIZE * SCALE, SIZE * SCALE), Image.NEAREST)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "store", "steam", "achievements")
    os.makedirs(out_dir, exist_ok=True)
    for name, spec in ACHIEVEMENTS.items():
        validate(name, spec)
        render(spec, locked=False).save(os.path.join(out_dir, f"{name}.png"))
        render(spec, locked=True).save(os.path.join(out_dir, f"{name}_locked.png"))
        print(f"  {name}: achieved + locked (64x64)")
    print(f"Wrote {len(ACHIEVEMENTS) * 2} icons to {out_dir}")


if __name__ == "__main__":
    main()
